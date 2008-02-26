/*
 *	outgoing.c: handle outgoing packets in the worker thread
 */

#include <string.h>

#include "outgoing.h"
#include "hlog.h"

void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c;
	struct sockaddr_in *c_in;
	struct sockaddr_in *pb_in;
	
	for (c = self->clients; (c); c = c->next) {
		/* do not send to the same client */
		if (c->addr.sa_family == AF_INET && pb->addr.sa_family == AF_INET) {
			c_in = (struct sockaddr_in *)&c->addr;
			pb_in = (struct sockaddr_in *)&pb->addr;
			if (c_in->sin_port == pb_in->sin_port
				&& c_in->sin_addr.s_addr == pb_in->sin_addr.s_addr)
					continue;
		}
		/* do not send to clients which are not logged in */
		if (c->state != CSTATE_CONNECTED)
			continue;
		
		/* TODO: process filters - check if this packet should be sent to this client */
		
		client_write(self, c, pb->data, pb->packet_len);
	}
}

