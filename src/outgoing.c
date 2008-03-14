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
		// FIXME: on production also CSTATE_UPLINK is to be permitted

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

		if ((pb->flags & F_DUPE) && !c->want_dupes) {
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


static void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c, *cnext;
	
	for (c = self->clients; (c); c = cnext) {
		cnext = c->next; // the client_write() MAY destroy the client object!

		/* Do not send to clients that are not logged in. */
		if (c->state != CSTATE_CONNECTED)
			continue;
		// FIXME: on production also CSTATE_UPLINK is to be permitted

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

		if ((pb->flags & F_DUPE) && !c->want_dupes) {
		  /* Duplicate packet.
		     Don't send, unless client especially wants! */
		  continue;
		}

		/*  Process filters - check if this packet should be
		    sent to this client.   */

		if (filter_process(self, c, pb) > 0) {
			client_write(self, c, pb->data, pb->packet_len);
		}
		/* negative results on filter_process() above may indicate
		   that the client object got destroyed!  The  cnext  at
		   the top of the loop saves our day.  */
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
#endif
