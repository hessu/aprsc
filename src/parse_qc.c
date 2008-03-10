/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

/*
 *	parse_qc.c: Process the APRS-IS Q construct
 */

#include <string.h>
#include <stdio.h>

#include "parse_qc.h"
#include "incoming.h"
#include "config.h"

/*
 *	q_dropcheck contains the last big block of the Q construct algorithm
 *	(All packets with q constructs...) and does rejection and duplicate
 *	dropping.
 */

int q_dropcheck(struct client_t *c, char *new_q, int new_q_size,
		int new_q_len, char q_proto, char q_type, char *q_start,
		char *path_end)
{
	char *p, *next, *next_end;
	int len;
	
	/*
	 * if ,qAZ, is the q construct:
	 * {
	 *	Dump to the packet to the reject log
	 *	Quit processing the packet
	 * }
	 */
	
	if (q_proto == 'A' && q_type == 'Z') {
		/* TODO: We could have a reject log here... */
		return -2; /* drop the packet */
	}
	
	/*
	 * If ,SERVERLOGIN is found after the q construct:
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 *
	 * (note: if serverlogin is 'XYZ-1', it must not match XYZ-12, so we need to
	 * match against ,SERVERLOGIN, or ,SERVERLOGIN:)
	 */
	
	len = strlen(mycall);
	p = memstr(mycall, q_start+4, path_end);
	if (p && *(p-1) == ',' && ( *(p+len) == ',' || p+len == path_end || *(p+len) == ':' )) {
		/* TODO: Should dump to a loop log... */
		return -2; /* drop the packet */
	}
	
	/*
	 * If a callsign-SSID is found twice in the q construct:
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 */
	p = q_start + 3;
	while (p < path_end) {
		/* find next ',' */
		p = memchr(p, ',', path_end - p);
		if (!p) /* no more... */
			break;
		
		p++; /* p should now point to the callsign */
		if (p >= path_end)
			break;
		
		/* find next callsign */
		next = memchr(p, ',', path_end - p);
		if (!next) /* no more callsigns */
			break;
		len = next - p;
		if (len == 0)
			continue; /* whee, found a ",," */
		
		/* loop through the rest of the callsigns */
		while ((next) && next < path_end) {
			next++;
			/* find end of the next callsign */
			next_end = memchr(next, ',', path_end - next);
			if (!next_end)
				next_end = path_end;
			
			if (next_end - next == 0)
				continue; /* whee, found a ",," */
			
			if (next_end - next == len && strncasecmp(p, next, len) == 0) {
				/* TODO: should dump to a loop log */
				return -2;
			}
			
			next = next_end;
		}
		
		p++;
	}
	
	/*
	 * If a verified login other than this login is found in the q construct
	 * and that login is not allowed to have multiple verified connects (the
	 * IPADDR of an outbound connection is considered a verified login):
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 */
	
	/*
	 * If the packet is from an inbound port and the login is found after the q construct but is not the LAST VIACALL:
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 */
	
	/*
	 * If trace is on, the q construct is qAI, or the FROMCALL is on the server's trace list:
	 * {
	 *	If the packet is from a verified port where the login is not found after the q construct:
	 *		Append ,login
	 *	else if the packet is from an outbound connection
	 *		Append ,IPADDR
	 *	
	 *	Append ,SERVERLOGIN
	 * }
	 */
	
	return new_q_len;
}


/*
 *	Parse, and possibly generate a Q construct.
 *	http://www.aprs-is.net/q.htm
 *	http://www.aprs-is.net/qalgorithm.htm
 *
 *	Called by incoming.c, runs in the worker thread's context.
 *	Threadsafe.
 *
 *	1) figure out where a (possibly) existing Q construct is
 *	2) if it exists, we might need to modify it
 *	3) we might have to append a new Q construct if one does not exist
 *	4) we might have to append our server ID to the path
 *	5) we might have to append the hexadecimal IP address of an UDP peer
 *
 *	This function is a bit too long, should probably split a bit, and
 *	reuse some code snippets.
 */

int q_process(struct client_t *c, char *new_q, int new_q_size, char *via_start, char **path_end, int pathlen, int originated_by_client)
{
	char *q_start = NULL; /* points to the , before the Q construct */
	char *q_nextcall = NULL; /* points to the , after the Q construct */ 
	char *q_nextcall_end = NULL; /* points to the , after the callsign right after the Q construct */
	int new_q_len = 0; /* the length of a newly generated Q construct */
	char q_proto = 0; /* parsed Q construct protocol character (A for APRS) */
	char q_type = 0; /* parsed Q construct type character */
	
	/*
	All packets
	{
		Place into TNC-2 format
		If a q construct is last in the path (no call following the qPT)
			delete the qPT
	}
	*/
	
	// fprintf(stderr, "q_process\n");
	q_start = memstr(",q", via_start, *path_end);
	if (q_start) {
		// fprintf(stderr, "\tfound existing q construct\n");
		/* there is an existing Q construct, check for a callsign after it */
		q_nextcall = memchr(q_start + 1, ',', *path_end - q_start - 1);
		if (!q_nextcall) {
			// fprintf(stderr, "\tno comma after Q construct, ignoring and overwriting construct\n");
			*path_end = q_start;
		} else if (q_nextcall - q_start != 4) {
			/* does not fit qPT */
			// fprintf(stderr, "\tlength of Q construct is not 3 characters\n");
			*path_end = q_start;
		} else {
			/* parse the q construct itself */
			q_proto = *(q_start + 2);
			q_type = *(q_start + 3);
			
			/* check for callsign following qPT */
			q_nextcall++; /* now points to the callsign */
			q_nextcall_end = q_nextcall;
			while (q_nextcall_end < *path_end && *q_nextcall_end != ',' && *q_nextcall_end != ':')
				q_nextcall_end++;
			if (q_nextcall == q_nextcall_end) {
				// fprintf(stderr, "\tno callsign after Q construct, ignoring and overwriting construct\n");
				*path_end = q_start;
				q_proto = q_type = 0; /* for the further code: we do not have a Qc */
			}
			/* it's OK to have more than one callsign after qPT */
		}
	}
	
	// fprintf(stderr, "\tstep 2...\n");

	/* ok, we now either have found an existing Q construct + the next callsign,
	 * or have eliminated an outright invalid Q construct.
	 */
	
	/*
	 * All packets from an inbound connection that would normally be passed per current validation algorithm:
	 */
	
	if (c->state == CSTATE_CONNECTED) {
		/*
		 * If the packet entered the server from an UDP port:
		 * {
		 *    if a q construct with a single call exists in the packet
		 *        Replace the q construct with ,qAU,SERVERLOGIN
		 *    else if more than a single call exists after the q construct
		 *        Invalid header, drop packet as error
		 *    else
		 *        Append ,qAU,SERVERLOGIN
		 *    Quit q processing
		 * }
		 */
		if (c->udp_port) {
			fprintf(stderr, "\tUDP packet\n");
			if (q_proto && q_nextcall_end == *path_end) {
				/* a q construct with a single call exists in the packet,
				 * Replace the q construct with ,qAU,SERVERLOGIN
				 */
				*path_end = q_start;
				return snprintf(new_q, new_q_size, ",qAU,%s", mycall);
			} else if (q_proto && q_nextcall_end < *path_end) {
				/* more than a single call exists after the q construct,
				 * invalid header, drop the packet as error
				 */
				return -1;
			} else {
				/* Append ,qAU,SERVERLOGIN */
				return snprintf(new_q, new_q_size, ",qAU,%s", mycall);
			}
		}
		
		/*
		 * If the packet entered the server from an unverified connection AND
		 * the FROMCALL equals the client login AND the header has been
		 * successfully converted to TCPXX format (per current validation algorithm):
		 * {
		 *    (All packets not deemed "OK" from an unverified connection should be dropped.)
		 *    if a q construct with a single call exists in the packet
		 *        Replace the q construct with ,qAX,SERVERLOGIN
		 *    else if more than a single call exists after the q construct
		 *        Invalid header, drop packet as error
		 *    else
		 *        Append ,qAX,SERVERLOGIN
		 *    Quit q processing
		 * }
		 */
		if (!c->validated && originated_by_client) {
			// fprintf(stderr, "\tunvalidated client sends packet originated by itself\n");
			// FIXME: how to check if TCPXX conversion is done? Just assume?
			if (q_proto && q_nextcall_end == *path_end) {
				/* a q construct with a single call exists in the packet,
				 * Replace the q construct with ,qAX,SERVERLOGIN
				 */
				*path_end = q_start;
				return snprintf(new_q, new_q_size, ",qAX,%s", mycall);
			} else if (q_proto && q_nextcall_end < *path_end) {
				/* more than a single call exists after the q construct,
				 * invalid header, drop the packet as error
				 */
				return -1;
			} else {
				/* Append ,qAX,SERVERLOGIN */
				return snprintf(new_q, new_q_size, ",qAX,%s", mycall);
			}
		}
		
		/*
		 * If the packet entered the server from a verified client-only connection
		 * AND the FROMCALL does not match the login:
		 * {
		 *	if a q construct exists in the packet
		 *		if the q construct is at the end of the path AND it equals ,qAR,login
		 *			Replace qAR with qAo
		 *	else if the path is terminated with ,I
		 *	{
		 *		if the path is terminated with ,login,I
		 *			Replace ,login,I with qAo,login
		 *		else
		 *			Replace ,VIACALL,I with qAr,VIACALL
		 *	}
		 *	else
		 *		Append ,qAO,login
		 *	Skip to "All packets with q constructs"
		 * }
		 */
		if (c->validated && !originated_by_client) {
			// FIXME: what is a "verified client-only connection?"
			// fprintf(stderr, "\tvalidated client sends sends packet originated by someone else\n");
			/* if a q construct exists in the packet */
			if (q_proto) {
				// fprintf(stderr, "\thas q construct\n");
				/* if the q construct is at the end of the path AND it equals ,qAR,login */
				if (q_proto == 'A' && q_type == 'R' && q_nextcall_end == *path_end) {
					/* Replace qAR with qAo */
					q_type = 'o';
					*(q_start + 3) = 'o';
					q_type = 'o';
					// fprintf(stderr, "\treplaced qAR with qAo\n");
				}
			} else if (pathlen > 2 && *(*path_end -1) == 'I' && *(*path_end -2) == ',') {
				// fprintf(stderr, "\tpath has ,I in the end\n");
				/* the path is terminated with ,I - lookup previous callsign in path */
				char *p = *path_end - 3;
				while (p > via_start && *p != ',')
					p--;
				if (*p == ',') {
					const char *prevcall = p+1;
					const char *prevcall_end = *path_end - 2;
					// fprintf(stderr, "\tprevious callsign is %.*s\n", prevcall_end - prevcall, prevcall);
					/* if the path is terminated with ,login,I */
					if (strlen(c->username) == prevcall_end - prevcall && strncasecmp(c->username, prevcall, prevcall_end - prevcall) == 0) {
						/* Replace ,login,I with qAo,login */
						*path_end = p;
						new_q_len = snprintf(new_q, new_q_size, ",qAo,%s", c->username);
						q_proto = 'A';
						q_type = 'o';
					} else {
						/* Replace ,VIACALL,I with qAr,VIACALL */
						*path_end = p;
						new_q_len = snprintf(new_q, new_q_size, ",qAr,%.*s", prevcall_end - prevcall, prevcall);
						q_proto = 'A';
						q_type = 'r';
					}
				} else {
					/* Undefined by the algorithm - there was no VIACALL */
					return -1;
				}
			} else {
				/* Append ,qAO,login */
				new_q_len = snprintf(new_q, new_q_size, ",qAO,%s", c->username);
				q_proto = 'A';
				q_type = 'O';
			}
			/* Skip to "All packets with q constructs" */
			return q_dropcheck(c, new_q, new_q_size, new_q_len, q_proto, q_type, q_start, *path_end);
		}
		
		/*
		 * If a q construct exists in the header:
		 *	Skip to "All packets with q constructs"
		 */
		if (q_proto) {
			// fprintf(stderr, "\texisting q construct\n");
			/* Skip to "All packets with q constructs" */
			return q_dropcheck(c, new_q, new_q_size, new_q_len, q_proto, q_type, q_start, *path_end);
		}
		
		/* At this point we have packets which do not have Q constructs, and
		 * are either (from validated clients && originated by client)
		 * or (from unvalidated client && not originated by the client)
		 */
		 
		/*
		 * If header is terminated with ,I:
		 * {
		 *	If the VIACALL preceding the ,I matches the login:
		 *		Change from ,VIACALL,I to ,qAR,VIACALL
		 *	Else
		 *		Change from ,VIACALL,I to ,qAr,VIACALL
		 * }
		 * Else If the FROMCALL matches the login:
		 * {
		 *	Append ,qAC,SERVERLOGIN
		 *	Quit q processing
		 * }
		 * Else
		 *	Append ,qAS,login
		 * Skip to "All packets with q constructs"
		 */
		if (pathlen > 2 && *(*path_end -1) == 'I' && *(*path_end -2) == ',') {
			// fprintf(stderr, "\tpath has ,I in the end\n");
			/* the path is terminated with ,I - lookup previous callsign in path */
			char *p = *path_end - 3;
			while (p > via_start && *p != ',')
				p--;
			if (*p == ',') {
				const char *prevcall = p+1;
				const char *prevcall_end = *path_end - 2;
				// fprintf(stderr, "\tprevious callsign is %.*s\n", prevcall_end - prevcall, prevcall);
				/* if the path is terminated with ,login,I */
				if (strlen(c->username) == prevcall_end - prevcall && strncasecmp(c->username, prevcall, prevcall_end - prevcall) == 0) {
					/* Replace ,login,I with qAR,login */
					*path_end = p;
					new_q_len = snprintf(new_q, new_q_size, ",qAR,%s", c->username);
					q_proto = 'A';
					q_type = 'R';
				} else {
					/* Replace ,VIACALL,I with qAr,VIACALL */
					*path_end = p;
					new_q_len = snprintf(new_q, new_q_size, ",qAr,%.*s", prevcall_end - prevcall, prevcall);
					q_proto = 'A';
					q_type = 'r';
				}
			} else {
				/* Undefined by the algorithm - there was no VIACALL */
				return -1;
			}
		} else if (originated_by_client) {
			/* FROMCALL matches the login */
			return snprintf(new_q, new_q_size, ",qAC,%s", mycall);
		} else {
			/* Append ,qAS,login */
			new_q_len = snprintf(new_q, new_q_size, ",qAS,%s", c->username);
			q_proto = 'A';
			q_type = 'S';
		}
		/* Skip to "All packets with q constructs" */
		return q_dropcheck(c, new_q, new_q_size, new_q_len, q_proto, q_type, q_start, *path_end);
	}
	
	/*
	 * If packet entered the server from an outbound connection (to
	 * another server's port 1313, for instance) and no q construct
	 * exists in the header:
	 * {
	 *	If header is terminated with ,I:
	 *		Change from ,VIACALL,I to ,qAr,VIACALL
	 *	Else
	 *		Append ,qAS,IPADDR (IPADDR is an 8 character hex
	 *		representation of the IP address of the remote server)
	 * }
	 * Untested at the time of implementation (no uplink support yet)
	 */
	
	if (!q_proto && (c->state == CSTATE_UPLINK || c->state == CSTATE_UPLINKSIM)) {
		if (pathlen > 2 && *(*path_end -1) == 'I' && *(*path_end -2) == ',') {
			// fprintf(stderr, "\tpath has ,I in the end\n");
			/* the path is terminated with ,I - lookup previous callsign in path */
			char *p = *path_end - 3;
			while (p > via_start && *p != ',')
				p--;
			if (*p == ',') {
				const char *prevcall = p+1;
				const char *prevcall_end = *path_end - 2;
				// fprintf(stderr, "\tprevious callsign is %.*s\n", prevcall_end - prevcall, prevcall);
				/* Replace ,VIACALL,I with qAr,VIACALL */
				*path_end = p;
				new_q_len = snprintf(new_q, new_q_size, ",qAr,%.*s", prevcall_end - prevcall, prevcall);
				q_proto = 'A';
				q_type = 'r';
			} else {
				/* Undefined by the algorithm - there was no VIACALL */
				return -1;
			}
		} else {
			/* Append ,qAS,IPADDR (IPADDR is an 8 character hex representation 
			 * of the IP address of the remote server)
			 * ... what should we do with IPv6? TODO: check what the competition does.
			 */
			/* FIXME: generate the hex IP addr
			 * new_q_len = snprintf(new_q, new_q_size, ",qAS,%s", ...);
			 */
			q_proto = 'A';
			q_type = 'S';
		}
	}
	
	return q_dropcheck(c, new_q, new_q_size, new_q_len, q_proto, q_type, q_start, *path_end);
}

