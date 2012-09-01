
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

#include "messaging.h"
#include "version.h"
#include "config.h"

/*
 *	Generate a message ID
 */

void messaging_generate_msgid(char *buf, int buflen)
{
	int i, c;
	
	for (i = 0; i < buflen-1; i++) {
		c = rand() % (2*26 + 10); /* letters and numbers */
		
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
 *	Ack an incoming message
 */
 
int messaging_ack(struct worker_t *self, struct client_t *c, struct pbuf_t *pb, struct aprs_message_t *am)
{
	return client_printf(self, c, "SERVER>" APRSC_TOCALL ",TCPIP*,qAZ,%s::%-9.*s:ack%.*s\r\n",
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
	
	return client_printf(self, c, "SERVER>" APRSC_TOCALL ",TCPIP*,qAZ,%s::%-9s:%s{%s\r\n",
		serverid, c->username, s, msgid);
}

