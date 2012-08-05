/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

/*
 *	parse_qc.c: Process the APRS-IS Q construct
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "parse_qc.h"
#include "incoming.h"
#include "clientlist.h"
#include "config.h"
#include "hlog.h"

static int check_invalid_q_callsign(const char *call, int len)
{
	const char *p = call;
	const char *e = call + len;
	
	while (p < e) {
		if (!isascii(*p))
			return -1;
		if ((!isalnum(*p)) && *p != '-')
			return -1;
		p++;
	}
	
	return 0;
}

/*
 *	q_dropcheck contains the last big block of the Q construct algorithm
 *	(All packets with q constructs...) and does rejection and duplicate
 *	dropping.
 */

#define MAX_Q_CALLS 64

static int q_dropcheck( struct client_t *c, const char *pdata, char *new_q, int new_q_size, char *via_start,
			int new_q_len, char q_proto, char q_type, char *q_start,
			char **q_replace, char *path_end )
{
	char *qcallv[MAX_Q_CALLS+1];
	int qcallc;
	char *p;
	int serverid_len, username_len;
	int login_in_path = 0;
	int i, j, l;
	
	/*
	 * if ,qAZ, is the q construct:
	 * {
	 *	Dump to the packet to the reject log
	 *	Quit processing the packet
	 * }
	 */
	
	if (q_proto == 'A' && q_type == 'Z') {
		/* TODO: The reject log should really log the offending packet */
		hlog(LOG_DEBUG, "dropping for unknown Q construct %c%c", q_proto, q_type);
		return -21; /* drop the packet */
	}
	
	/*
	 * Produce an array of pointers pointing to each callsign in the path
	 * after the q construct
	 */
	qcallc = 0;
	if (q_start) {
		p = q_start + 4;
		while (qcallc < MAX_Q_CALLS && p < path_end) {
			while (p < path_end && *p == ',')
				p++;
			if (p == path_end)
				break;
			qcallv[qcallc++] = p;
			while (p < path_end && *p != ',')
				p++;
		}
		qcallv[qcallc] = p+1;
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
	
	if (q_start) {
		serverid_len = strlen(serverid);
		p = memstr(serverid, q_start+4, path_end);
		if (p && *(p-1) == ',' && ( *(p+serverid_len) == ',' || p+serverid_len == path_end || *(p+serverid_len) == ':' )) {
			/* TODO: The reject log should really log the offending packet */
			hlog(LOG_DEBUG, "dropping due to my callsign appearing in path");
			return -21; /* drop the packet */
		}
	}
	
	/*
	 * 1)
	 * If a callsign-SSID is found twice in the q construct:
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 *
	 * 2)
	 * If a verified login other than this login is found in the q construct
	 * and that login is not allowed to have multiple verified connects (the
	 * IPADDR of an outbound connection is considered a verified login):
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 *
	 * 3)
	 * If the packet is from an inbound port and the login is found after the q construct but is not the LAST VIACALL:
	 * {
	 *	Dump to the loop log with the sender's IP address for identification
	 *	Quit processing the packet
	 * }
	 *
	 */
	username_len = strlen(c->username);
	for (i = 0; i < qcallc; i++) {
		l = qcallv[i+1] - qcallv[i] - 1;
		/* 1) */
		for (j = i + 1; j < qcallc; j++) {
			/* this match is case sensitive in javaprssrvr, so that's what we'll do */
			if (l == qcallv[j+1] - qcallv[j] - 1 && strncmp(qcallv[i], qcallv[j], l) == 0) {
				/* TODO: The reject log should really log the offending packet */
				hlog(LOG_DEBUG, "dropping due to callsign-SSID '%.*s' found twice after Q construct", l, qcallv[i]);
			    	return -21;
			}
		}
		if (l == username_len && strncasecmp(qcallv[i], c->username, username_len) == 0) {
			/* ok, login is client's login, handle step 3) */
			login_in_path = 1;
			if (c->state == CSTATE_CONNECTED &&
			    (c->flags & CLFLAGS_INPORT) &&
			    (i != qcallc - 1)) {
				/* 3) hits: from an inbound connection, client login found in path,
				 * but is not the last viacall
				 * TODO: should dump...
				 */
				/* TODO: The reject log should really log the offending packet */
				hlog(LOG_DEBUG, "dropping due to login callsign %s not being the last viacall after Q construct", c->username);
				return -21;
			}
		} else if (clientlist_check_if_validated_client(qcallv[i], l) != -1) {
			/* 2) hits: TODO: should dump to a loop log */
			/* TODO: The reject log should really log the offending packet */
			hlog(LOG_DEBUG, "dropping due to callsign '%.*s' after Q construct being logged in on another socket, arrived from %s", l, qcallv[i], c->username);
			return -21;
		} else if (check_invalid_q_callsign(qcallv[i], l)) {
			hlog(LOG_DEBUG, "dropping due to callsign '%.*s' after Q construct being invalid as an APRS-IS server name, arrived from %s", l, qcallv[i], c->username);
			return -21;
		}
		
		if (q_type == 'U' && memcmp(qcallv[i], pdata, l) == 0 && pdata[l] == '>') {
			hlog(LOG_DEBUG, "dropping due to callsign '%.*s' after qAU also being srccall", l, qcallv[i]);
			return -21;
		}
	}
	
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
	
	if (q_proto == 'A' && q_type == 'I') {
		/* we replace the existing Q construct with a regenerated one */
		*q_replace = q_start+1;
		
		/* copy over existing qAI trace */
		new_q_len = path_end - q_start - 1;
		//hlog(LOG_DEBUG, "qAI replacing, new_q_len %d", new_q_len);
		if (new_q_len > new_q_size) {
			/* ouch, memcpy would run over the buffer */
			/* TODO: The reject log should really log the offending packet */
			hlog(LOG_DEBUG, "dropping due to buffer being too tight");
			return -21;
		}
		memcpy(new_q, q_start+1, new_q_len);
		
		//hlog(LOG_DEBUG, "qAI first memcpy done, new_q_len %d, q_replace %d", new_q_len, *q_replace);
		
		/* If the packet is from a verified port where the login is not found after the q construct */
		if (c->validated && !login_in_path) {
			/* Append ,login */
			new_q_len += snprintf(new_q + new_q_len, new_q_size - new_q_len, ",%s", c->username);
		} else if (!(c->flags & CLFLAGS_INPORT) && !login_in_path) {
			/* from an outbound connection, append client's hexaddr */
			//hlog(LOG_DEBUG, "qAI appending hex address, starting at %d, got %d left in buffer", new_q_len, new_q_size - new_q_len);
			new_q_len += snprintf(new_q + new_q_len, new_q_size - new_q_len, ",%s", c->username);
		}
		//hlog(LOG_DEBUG, "qAI append done, new_q_len %d, new_q_size %d, q_replace %d, going to append %d more", new_q_len, new_q_size, *q_replace, strlen(serverid)+1);
		
		if (new_q_size - new_q_len < 20) {
			hlog(LOG_DEBUG, "dropping due to buffer being too tight when appending my login for qAI");
			return -21;
		}
		
		/* Append ,SERVERLOGIN */
		new_q_len += snprintf(new_q + new_q_len, new_q_size - new_q_len, ",%s", serverid);
	}
	
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

int q_process(struct client_t *c, const char *pdata, char *new_q, int new_q_size, char *via_start,
              char **path_end, int pathlen, char **new_q_start, char **q_replace,
              int originated_by_client)
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
	} else {
		/* if the packet's srccall == login, replace digipeater path with
		 * TCPIP* and insert Q construct
		 */
		//hlog(LOG_DEBUG, "no q found");
		if (originated_by_client) {
			// where to replace from
			*q_replace = via_start;
			//hlog(LOG_DEBUG, "inserting TCPIP,qAC... starting at %s", *q_replace);
			return snprintf(new_q, new_q_size, ",TCPIP*,qAC,%s", serverid);
		}
	}
	
	// fprintf(stderr, "\tstep 2...\n");

	/* ok, we now either have found an existing Q construct + the next callsign,
	 * or have eliminated an outright invalid Q construct.
	 */
	
	/*
	 * All packets from an inbound connection that would normally be passed per current validation algorithm:
	 */
	
	if (c->state == CSTATE_CONNECTED &&
	    (c->flags & CLFLAGS_INPORT)) {
		/*
		 * If the packet entered the server from an UDP port:
		 * {
		 *    if a q construct exists in the packet
		 *        Replace the q construct with ,qAU,SERVERLOGIN
		 *    else
		 *        Append ,qAU,SERVERLOGIN
		 *    Quit q processing
		 * }
		 */
		if (c->udp_port) {
			//fprintf(stderr, "\tUDP packet\n");
			if (q_proto) {
				/* a q construct with a exists in the packet,
				 * Replace the q construct with ,qAU,SERVERLOGIN
				 */
				*path_end = q_start;
				return snprintf(new_q, new_q_size, ",qAU,%s", serverid);
			} else {
				/* Append ,qAU,SERVERLOGIN */
				return snprintf(new_q, new_q_size, ",qAU,%s", serverid);
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
		if ((!c->validated) && originated_by_client) {
			// fprintf(stderr, "\tunvalidated client sends packet originated by itself\n");
			// FIXME: how to check if TCPXX conversion is done? Just assume?
			if (q_proto && q_nextcall_end == *path_end) {
				/* a q construct with a single call exists in the packet,
				 * Replace the q construct with ,qAX,SERVERLOGIN
				 */
				*path_end = via_start;
				return snprintf(new_q, new_q_size, ",TCPXX*,qAX,%s", serverid);
			} else if (q_proto && q_nextcall_end < *path_end) {
				/* more than a single call exists after the q construct,
				 * invalid header, drop the packet as error
				 */
				return -20;
			} else {
				/* Append ,qAX,SERVERLOGIN (well, javaprssrvr replaces the via path too) */
				*path_end = via_start;
				return snprintf(new_q, new_q_size, ",TCPXX*,qAX,%s", serverid);
			}
		}
		
		/* OLD:
		 * If the packet entered the server from a verified client-only connection
		 * AND the FROMCALL does not match the login:
		 * {
		 *	if a q construct exists in the packet
		 *		if the q construct is at the end of the path AND it equals ,qAR,login
		 *			Replace qAR with qAo
		 *      else if the path is terminated with ,login,I
		 *      	Replace ,login,I with qAo,login
		 *	else
		 *		Append ,qAO,login
		 *	Skip to "All packets with q constructs"
		 * }
		 *
		 * NOW:
		 * If the packet entered the server from a verified client-only connection
		 * AND the FROMCALL does not match the login:
		 *     {
		 *         if a q construct exists in the path
		 *             if the q construct equals ,qAR,callsignssid or ,qAr,callsignssid
		 *                 Replace qAR or qAr with qAo
		 *             else if the q construct equals ,qAS,callsignssid
		 *                 Replace qAS with qAO
		 *             else if the q construct equals ,qAC,callsignssid and callsignssid is not equal to the servercall or login
		 *                 Replace qAC with qAO
		 *         else if the path is terminated with ,callsignssid,I
		 *             Replace ,callsignssid,I with qAo,callsignssid
		 *         else
		 *             Append ,qAO,login
		 *         Skip to "All packets with q constructs"
		 *     }
		 */
		if (c->validated && (c->flags & CLFLAGS_CLIENTONLY) && !originated_by_client) {
			// fprintf(stderr, "\tvalidated client sends sends packet originated by someone else\n");
			/* if a q construct exists in the packet */
			int add_qAO = 1;
			if (q_proto) {
				// fprintf(stderr, "\thas q construct\n");
				/* if the q construct equals ,qAR,callsignssid or ,qAr,callsignssid */
				if (q_proto == 'A' && (q_type == 'R' || q_type == 'r')) {
					/* Replace qAR with qAo */
					*(q_start + 3) = q_type = 'o';
					// fprintf(stderr, "\treplaced qAR with qAo\n");
				} else if (q_proto == 'A' && q_type == 'S') {
					/* Replace qAS with qAO */
					*(q_start + 3) = q_type = 'O';
					// fprintf(stderr, "\treplaced qAS with qAO\n");
				} else if (q_proto == 'A' && q_type == 'C') {
					// FIXME: should also check callsignssid to servercall & login
					/* Replace qAC with qAO */
					*(q_start + 3) = q_type = 'O';
					// fprintf(stderr, "\treplaced qAC with qAO\n");
				} else {
					// What? Dunno.
				}
				/* Not going to modify the construct, update pointer to it */
				*new_q_start = q_start + 1;
				add_qAO = 0;
			} else if (pathlen > 2 && *(*path_end -1) == 'I' && *(*path_end -2) == ',') {
				hlog(LOG_DEBUG, "path has ,I in the end: %s", pdata);
				/* the path is terminated with ,I - lookup previous callsign in path */
				char *p = *path_end - 3;
				while (p > via_start && *p != ',')
					p--;
				if (*p == ',') {
					const char *prevcall = p+1;
					const char *prevcall_end = *path_end - 2;
					hlog(LOG_DEBUG, "previous callsign is %.*s\n", (int)(prevcall_end - prevcall), prevcall);
					/* if the path is terminated with ,login,I */
					// TODO: Should validate that prevcall is a nice callsign
					if (1) {
						/* Replace ,login,I with qAo,previouscall */
						*path_end = p;
						new_q_len = snprintf(new_q, new_q_size, ",qAo,%.*s", (int)(prevcall_end - prevcall), prevcall);
						q_proto = 'A';
						q_type = 'o';
						add_qAO = 0;
					}
				}
			}
			
			if (add_qAO) {
				/* Append ,qAO,login */
				new_q_len = snprintf(new_q, new_q_size, ",qAO,%s", c->username);
				q_proto = 'A';
				q_type = 'O';
			}
			
			/* Skip to "All packets with q constructs" */
			return q_dropcheck(c, pdata, new_q, new_q_size, via_start, new_q_len, q_proto, q_type, q_start, q_replace, *path_end);
		}
		
		/*
		 * If a q construct exists in the header:
		 *	Skip to "All packets with q constructs"
		 */
		if (q_proto) {
			// fprintf(stderr, "\texisting q construct\n");
			/* Not going to modify the construct, update pointer to it */
			*new_q_start = q_start + 1;
			/* Skip to "All packets with q constructs" */
			return q_dropcheck(c, pdata, new_q, new_q_size, via_start, new_q_len, q_proto, q_type, q_start, q_replace, *path_end);
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
					new_q_len = snprintf(new_q, new_q_size, ",qAr,%.*s", (int)(prevcall_end - prevcall), prevcall);
					q_proto = 'A';
					q_type = 'r';
				}
			} else {
				/* Undefined by the algorithm - there was no VIACALL */
				return -20;
			}
		} else if (originated_by_client) {
			/* FROMCALL matches the login */
			/* Add TCPIP* in the end of the path only if it's not there already */
			if (pathlen > 7 && strncmp(*path_end-7, ",TCPIP*", 7) == 0)
				return snprintf(new_q, new_q_size, ",qAC,%s", serverid);
			else
				return snprintf(new_q, new_q_size, ",TCPIP*,qAC,%s", serverid);
		} else {
			/* Append ,qAS,login */
			new_q_len = snprintf(new_q, new_q_size, ",qAS,%s", c->username);
			q_proto = 'A';
			q_type = 'S';
		}
		/* Skip to "All packets with q constructs" */
		return q_dropcheck(c, pdata, new_q, new_q_size, via_start, new_q_len, q_proto, q_type, q_start, q_replace, *path_end);
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
	
	if (!q_proto && (c->flags & (CLFLAGS_UPLINKPORT|CLFLAGS_UPLINKSIM))) {
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
				new_q_len = snprintf(new_q, new_q_size, ",qAr,%.*s", (int)(prevcall_end - prevcall), prevcall);
				q_proto = 'A';
				q_type = 'r';
			} else {
				/* Undefined by the algorithm - there was no VIACALL */
				return -20;
			}
		} else {
			/* Append ,qAS,IPADDR (IPADDR is an 8 character hex representation 
			 * of the IP address of the remote server)
			 */
			new_q_len = snprintf(new_q, new_q_size, ",qAS,%s", c->addr_hex);
			q_proto = 'A';
			q_type = 'S';
		}
	}
	
	/* If we haven't generated a new Q construct, return a pointer to the existing one */
	if (!new_q_len) {
		if (q_start == NULL)
			hlog(LOG_ERR, "q: Did not find or generate a Q construct (from client %s fd %d): %s", c->username, c->fd, pdata);
		*new_q_start = q_start + 1;
	}
	
	return q_dropcheck(c, pdata, new_q, new_q_size, via_start, new_q_len, q_proto, q_type, q_start, q_replace, *path_end);
}

