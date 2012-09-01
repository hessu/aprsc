
/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef MESSAGING_H
#define MESSAGING_H

#include "worker.h"
#include "parse_aprs.h"

extern void messaging_generate_msgid(char *buf, int buflen);
extern int messaging_ack(struct worker_t *self, struct client_t *c, struct pbuf_t *pb, struct aprs_message_t *am);

#endif
