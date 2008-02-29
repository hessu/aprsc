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
 *	A simple APRS parser for aprsc. Translated from Ham::APRS::FAP
 *	perl module (by OH2KKU).
 *
 *	Only needs to get lat/lng out of the packet, other features would
 *	be unnecessary in this application, and slow down the parser.
 *	
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "parse_aprs.h"
#include "hlog.h"
#include "filter.h"

int parse_aprs_mice(struct pbuf_t *pb)
{
	fprintf(stderr, "parse_aprs_mice\n");
	return 0;
}

int parse_aprs_compressed(struct pbuf_t *pb, char *body, const char *body_end)
{
	fprintf(stderr, "parse_aprs_compressed\n");
	return 0;
}

int parse_aprs_uncompressed(struct pbuf_t *pb, char *body, const char *body_end)
{
	char posbuf[20];
	unsigned int lat_deg = 0, lat_min = 0, lat_min_frag = 0, lng_deg = 0, lng_min = 0, lng_min_frag = 0;
	float lat, lng;
	char lat_hemi, lng_hemi;
	char sym_table, sym_code;
	int issouth = 0;
	int iswest = 0;
	
	fprintf(stderr, "parse_aprs_uncompressed\n");
	
	if (body_end - body < 19) {
		fprintf(stderr, "\ttoo short\n");
		return 0;
	}
	
	/* make a local copy, so we can overwrite it at will. */
	memcpy(posbuf, body, 19);
	posbuf[19] = 0;
	fprintf(stderr, "\tposbuf: %s\n", posbuf);
	
	/* position ambiquity is going to get ignored now, it's not needed in this application. */
	/* lat */
	if (posbuf[2] == ' ') posbuf[2] = '3';
	if (posbuf[3] == ' ') posbuf[3] = '5';
	if (posbuf[5] == ' ') posbuf[5] = '5';
	if (posbuf[6] == ' ') posbuf[6] = '5';
	/* lng */
	if (posbuf[12] == ' ') posbuf[12] = '3';
	if (posbuf[13] == ' ') posbuf[13] = '5';
	if (posbuf[15] == ' ') posbuf[15] = '5';
	if (posbuf[16] == ' ') posbuf[16] = '5';
	
	fprintf(stderr, "\tafter filling amb: %s\n", posbuf);
	/* 3210.70N/13132.15E# */
	//if (sscanf(posbuf, "%2u%2u.%2u%c%c%3u%2u.%2u%c%c",
	if (sscanf(posbuf, "%2u%2u.%2u%c%c%3u%2u.%2u%c%c",
	    &lat_deg, &lat_min, &lat_min_frag, &lat_hemi, &sym_table,
	    &lng_deg, &lng_min, &lng_min_frag, &lng_hemi, &sym_code) != 10) {
		fprintf(stderr, "\tsscanf failed\n");
		return 0;
	}
	
	if (lat_hemi == 'S' || lat_hemi == 's')
		issouth = 1;
	else if (lat_hemi != 'N' && lat_hemi != 'n')
		return 0; /* neither north or south? bail out... */
	
	if (lng_hemi == 'W' || lng_hemi == 'w')
		iswest = 1;
	else if (lng_hemi != 'E' && lng_hemi != 'e')
		return 0; /* neither west or east? bail out ... */
	
	if (lat_deg > 89 || lng_deg > 179)
		return 0; /* too large values for lat/lng degrees */
	
	lat = (float)lat_deg + (float)lat_min / 60.0 + (float)lat_min_frag / 100.0 / 60.0;
	lng = (float)lng_deg + (float)lng_min / 60.0 + (float)lng_min_frag / 100.0 / 60.0;
	
	/* Finally apply south/west indicators */
	if (issouth)
		lat = 0 - lat;
	if (iswest)
		lng = 0 - lng;
	
	fprintf(stderr, "\tlat %u %u.%u %c (%.3f) lng %u %u.%u %c (%.3f)\n",
		lat_deg, lat_min, lat_min_frag, (int)lat_hemi, lat,
		lng_deg, lng_min, lng_min_frag, (int)lng_hemi, lng);


	/* Pre-calculations for A/R/F/M-filter tests */
	
	pb->lat = filter_lat2rad(lat);	/* deg-to-radians */
	pb->cos_lat = 0.0;		/* prefill the cache */

	pb->lng = filter_lon2rad(lng);	/* deg-to-radians */
	
	pb->packettype |= T_POSITION;	/* the packet has positional data */

	return 1;
}

int parse_aprs_object(struct pbuf_t *pb, char *body, const char *body_end)
{
	fprintf(stderr, "parse_aprs_object\n");
	
	return 0;
}

int parse_aprs_item(struct pbuf_t *pb, char *body, const char *body_end)
{
	fprintf(stderr, "parse_aprs_item\n");
	
	return 0;
}


/*
 *	Try to parse an APRS packet.
 *	Returns 1 if position was parsed successfully,
 *	0 if parsing failed.
 */

int parse_aprs(struct worker_t *self, struct pbuf_t *pb)
{
	char packettype, poschar;
	int paclen;
	char *body;
	char *body_end;
	char *pos_start;
	
	if (!pb->info_start)
		return 0;

	pb->packettype = 0;
	pb->flags      = 0;
	
	/* the following parsing logic has been translated from Ham::APRS::FAP
	 * Perl module to C
	 */
	
	/* length of the info field: length of the packet - length of header - CRLF */
	paclen = pb->packet_len - (pb->info_start - pb->data) - 2;
	/* Check the first character of the packet and determine the packet type */
	packettype = *pb->info_start;
	
	/* failed parsing */
	fprintf(stderr, "parse_aprs (%d):\n", paclen);
	fwrite(pb->info_start, paclen, 1, stderr);
	fprintf(stderr, "\n");
	
	/* body is right after the packet type character */
	body = pb->info_start + 1;
	/* ignore the CRLF in the end of the body */
	body_end = pb->data + pb->packet_len - 2;
	
	if (packettype == 0x27 || packettype == 0x60) {
		/* could be mic-e, minimum body length 9 chars */
		if (paclen >= 9)
			return parse_aprs_mice(pb);
	} else if (packettype == '!' || packettype == '=' || packettype == '/'
		|| packettype == '@') {
		/* check that we won't run over right away */
		if (body_end - body < 10)
			return -1;
		/* Normal or compressed location packet, with or without
		 * timestamp, with or without messaging capability
		 *
		 * ! and / have messaging, / and @ have a prepended timestamp
		 */
		if (packettype == '/' || packettype == '@') {
			/* With a prepended timestamp, jump over it. */
			body += 7;
		}
		poschar = *body;
		if (poschar == '/' || poschar == '\\' || (poschar >= 0x41 && poschar <= 0x5A)
		    || (poschar >= 0x61 && poschar <= 0x6A)) { /* [\/\\A-Za-j] */
		    	/* compressed position packet */
			if (body_end - body >= 13) {
				return parse_aprs_compressed(pb, body, body_end);
			}
			return 0;
			
		} else if (poschar >= 0x30 && poschar <= 0x39) { /* [0-9] */
			/* normal uncompressed position */
			if (body_end - body >= 19) {
				return parse_aprs_uncompressed(pb, body, body_end);
			}
			return 0;
		}
		
	} else if (packettype == ';') {
		if (body_end - body > 18) {
			return parse_aprs_object(pb, body, body_end);
		}
		
	} else if (packettype == ')') {
		if (body_end - body > 18) {
			return parse_aprs_item(pb, body, body_end);
		}
		
	} else {
		/* When all else fails, try to look for a !-position that can
		 * occur anywhere within the 40 first characters according
		 * to the spec.
		 */
		pos_start = memchr(body, '!', body_end - body);
		if ((pos_start) && pos_start - body <= 39) {
			poschar = *pos_start;
			if (poschar == '/' || poschar == '\\' || (poschar >= 0x41 && poschar <= 0x5A)
			    || (poschar >= 0x61 && poschar <= 0x6A)) { /* [\/\\A-Za-j] */
			    	/* compressed position packet */
			    	if (body_end - pos_start >= 13) {
			    		return parse_aprs_compressed(pb, pos_start, body_end);
				}
				return 0;
			} else if (poschar >= 0x30 && poschar <= 0x39) { /* [0-9] */
				/* normal uncompressed position */
				if (body_end - pos_start >= 19) {
					return parse_aprs_uncompressed(pb, pos_start, body_end);
				}
				return 0;
			}
		}
	}
	
	return 0;
}
