
#include "parse_aprs.h"
#include "hlog.h"

int parse_aprs_mice(struct pbuf_t *pb)
{
	return 0;
}

int parse_aprs_compressed(struct pbuf_t *pb, char *body, const char *body_end)
{
	return 0;
}

int parse_aprs_uncompressed(struct pbuf_t *pb, char *body, const char *body_end)
{
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
	char *body_end = pb->data + pb->packet_len - 2;
	
	if (!pb->info_start)
		return 0;
	
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
	
	if (packettype == 0x27 || packettype == 0x60) {
		/* could be mic-e, minimum body length 9 chars */
		if (paclen >= 9)
			return parse_aprs_mice(pb);
	} else if (packettype == '!' || packettype == '=' || packettype == '/'
		|| packettype == '@') {
		/* body is right after the packet type character */
		body = pb->info_start + 1;
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
		
	}
	
	return 0;
}
