/*
 *	outgoing.c: handle outgoing packets in the worker thread
 */

#include <string.h>

#include "outgoing.h"
#include "hlog.h"
#include "filter.h"

void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c;
	
	for (c = self->clients; (c); c = c->next) {

		/* Do not send to clients that are not logged in. */
		if (c->state != CSTATE_CONNECTED)
			continue;
		

		/* Do not send to the same client.
		   This may reject a packet that came from a socket that got
		   closed a few milliseconds previously and its client_t got
		   recycled on a newly connected client, but if the new client
		   is a long living one, all further packets will be accepted
		   just fine.
		   For packet history dumps this test shall be ignored!
		 */
		if (c == pb->origin)
			continue;

		/*  Process filters - check if this packet should be
		    sent to this client.   */

		if (filter_process(c, pb)) {
			client_write(self, c, pb->data, pb->packet_len);
		}
	}
}

