/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef PARSE_APRS_H
#define PARSE_APRS_H

#include "worker.h"
#include "parse_aprs.h"

struct aprs_message_t {
	const char *body;          /* message body */
	const char *msgid;
	
	int body_len;
	int msgid_len;
	int is_ack;
};

extern int parse_aprs(struct pbuf_t *pb);
extern int parse_aprs_message(struct pbuf_t *pb, struct aprs_message_t *am);

#endif
