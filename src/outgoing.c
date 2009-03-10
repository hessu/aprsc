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
 *	outgoing.c: handle outgoing packets in the worker thread
 */

#include <string.h>
#include <stdlib.h>

#include "outgoing.h"
#include "hlog.h"
#include "filter.h"

#if 1

static void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c, *cnext;
	
	for (c = self->clients; (c); c = cnext) {
		cnext = c->next; // the client_write() MAY destroy the client object!

		/* Do not send to clients that are not logged in. */
		if (c->state != CSTATE_CONNECTED)
			continue;
		if (c->flags & CLFLAGS_PORT_RO)
			continue;

		/* Do not send packet back to the source client.
		   This may reject a packet that came from a socket that got
		   closed a few milliseconds previously and its client_t got
		   recycled on a newly connected client, but if the new client
		   is a long living one, all further packets will be accepted
		   just fine.
		   For packet history dumps this test shall be ignored!
		 */
		if (c == pb->origin)
			continue;

		if ((pb->flags & F_DUPE) && (!(c->flags & CLFLAGS_DUPEFEED))) {
		  /* Duplicate packet.
		     Don't send, unless client especially wants! */
		  continue;
		}

		/*  Process filters - check if this packet should be
		    sent to this client.   */

		if (filter_process(self, c, pb) > 0) {
			client_write(self, c, pb->data, pb->packet_len);
		}
	}
}


/*
 *	Process outgoing packets, write them to clients
 */

void process_outgoing(struct worker_t *self)
{
	struct pbuf_t *pb;
	int e;
	
	if ((e = rwl_rdlock(&pbuf_global_rwlock))) {
		hlog(LOG_CRIT, "worker: Failed to rdlock pbuf_global_rwlock!");
		exit(1);
	}
	while ((pb = *self->pbuf_global_prevp)) {
		process_outgoing_single(self, pb);
		self->last_pbuf_seqnum = pb->seqnum;
		self->pbuf_global_prevp = &pb->next;
	}
	while ((pb = *self->pbuf_global_dupe_prevp)) {
		process_outgoing_single(self, pb);
		self->last_pbuf_dupe_seqnum = pb->seqnum;
		self->pbuf_global_dupe_prevp = &pb->next;
	}
	if ((e = rwl_rdunlock(&pbuf_global_rwlock))) {
		hlog(LOG_CRIT, "worker: Failed to rdunlock pbuf_global_rwlock!");
		exit(1);
	}
}

#else


/*
 *	Process outgoing packets, write them to clients
 *
 *	Alternate format, different clients can have different pb->seqnum
 *	feed under way..
 *	(FIXME: error handling vs. true "now buffer is full, please wait.")
 *	(FIXME: self->last_pbuf_(dupe_)seqnum management - must track largest lag )
 */

static int process_outgoing_client(struct worker_t *self, struct client_t *c)
{
	struct pbuf_t *pb;
	int e;
	
	while ((pb = *c->pbuf_global_prevp)) {
		c->last_pbuf_seqnum = pb->seqnum;
		c->pbuf_global_prevp = &pb->next;

		/* Do not send packet back to the source client.
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

		if (filter_process(self, c, pb) > 0) {
			if (client_write(self, c, pb->data, pb->packet_len) < 0)
				return -1;
		}
	}


	if (c->flags & CLFLAGS_DUPEFEED) {
		/* Duplicate packet, and client especially wants them! */
		while ((pb = *c->pbuf_global_dupe_prevp)) {
			c->last_pbuf_dupe_seqnum = pb->seqnum;
			c->pbuf_global_dupe_prevp = &pb->next;

		/* Do not send packet back to the source client.
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

			if (filter_process(self, c, pb) > 0) {
				if (client_write(self, c, pb->data, pb->packet_len) < 0)
					return -1;
			}
		}
	} else {
		/* Client does not want dupe packets, just set values of last
		   packet on dupe chain  */
		if (pbuf_global_dupe_last)
			c->last_pbuf_dupe_seqnum = pbuf_global_dupe_last->seqnum;
		c->pbuf_global_dupe_prevp = pbuf_global_dupe_prevp;
	}
	return 0;
}


static void process_outgoing(struct worker_t *self)
{
	struct client_t *c, *cnext;
	
	if ((e = rwl_rdlock(&pbuf_global_rwlock))) {
		hlog(LOG_CRIT, "worker: Failed to rdlock pbuf_global_rwlock!");
		exit(1);
	}

	for (c = self->clients; (c); c = cnext) {
		cnext = c->next; // the client_write() MAY destroy the client object!

		/* Do not send to clients that are not logged in. */
		if ( c->state != CSTATE_CONNECTED )
			continue;
		if ( c->flags & CLFLAGS_PORT_RO ) // not to r/o ports..
			continue;
		if ( process_outgoing_client(self, c) < 0 )
			continue; // Maybe the client got destroyed
		// FIXME: self->pbuf_last_(dupe_)seqnum tracking - largest lag
	}

	if ((e = rwl_rdunlock(&pbuf_global_rwlock))) {
		hlog(LOG_CRIT, "worker: Failed to rdunlock pbuf_global_rwlock!");
		exit(1);
	}
}


#endif
