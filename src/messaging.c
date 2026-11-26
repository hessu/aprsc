
/*
 *     aprsc
 *
 *     (c) Heikki Hannikainen, OH7LZB
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 *
 *     The messaging module implements utility functions for processing and
 *     generating APRS messages.
 */


#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "messaging.h"
#include "version.h"
#include "config.h"
#include "aprsis2.h"
#include "hlog.h"

/*
 *	Generate a message ID
 */

void messaging_generate_msgid(char *buf, int buflen)
{
	int i, c;
	
	for (i = 0; i < buflen-1; i++) {
		// coverity[dont_call]  // squelch warning: not security sensitive use of random(): APRS message-id
		c = random() % (2*26 + 10); /* letters and numbers */
		
		if (c < 10)
			buf[i] = c + 48; /* number */
		else if (c < 26+10)
			buf[i] = c - 10 + 97; /* lower-case letter */
		else
			buf[i] = c - 36 + 65; /* upper-case letter */
	}
	
	/* null-terminate */
	buf[i] = 0;
}


/*
 *	Send a packet made by the server to a client: as a line to
 *	a plaintext client, and as an IS packet to an IS2 one, which
 *	a line of text would break the framing of.
 *	Returns a value < -2 if the client was destroyed.
 */

static int messaging_send(struct worker_t *self, struct client_t *c, const char *fmt, ...) PRINTF_LIKE(3, 4);

static int messaging_send(struct worker_t *self, struct client_t *c, const char *fmt, ...)
{
	va_list args;
	char s[PACKETLEN_MAX];
	int i;
	
	va_start(args, fmt);
	i = vsnprintf(s, PACKETLEN_MAX-2, fmt, args);
	va_end(args);
	
	/* room is left for the CRLF */
	if (i < 0 || i >= PACKETLEN_MAX-2) {
		hlog(LOG_ERR, "messaging_send vsnprintf failed to %s: '%s'", c->addr_rem, fmt);
		return -1;
	}
	
	if ((c->flags & CLFLAGS_IS2) || c->corepeer_is2)
		return is2_write_packet_data(self, c, s, i);
	
	memcpy(s + i, "\r\n", 2);
	
	return c->write(self, c, s, i + 2);
}

/*
 *	Ack an incoming message
 */
 
int messaging_ack(struct worker_t *self, struct client_t *c, struct pbuf_t *pb, struct aprs_message_t *am)
{
	return messaging_send(self, c, "SERVER>" APRSC_TOCALL ",TCPIP*,qAZ,%s::%-9.*s:ack%.*s",
		serverid, pb->srcname_len, pb->srcname, am->msgid_len, am->msgid);
}

/*
 *	Send a message to a local logged-in client
 */

extern int messaging_message_client(struct worker_t *self, struct client_t *c, const char *fmt, ...)
{
	va_list args;
	char s[PACKETLEN_MAX];
	char msgid[5];
	
	va_start(args, fmt);
	vsnprintf(s, PACKETLEN_MAX, fmt, args);
	va_end(args);
	
	messaging_generate_msgid(msgid, sizeof(msgid));
	
	return messaging_send(self, c, "SERVER>" APRSC_TOCALL ",TCPIP*,qAZ,%s::%-9s:%s{%s",
		serverid, c->username, s, msgid);
}

