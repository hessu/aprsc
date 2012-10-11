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

/*
 *	send a single packet to all clients (and peers and uplinks) which
 *	should have a copy
 */

static inline void send_single(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	if (c->udp_port && c->udpclient)
		clientaccount_add( c, IPPROTO_UDP, 0, 0, 0, 1, 0);
	else
		clientaccount_add( c, IPPROTO_TCP, 0, 0, 0, 1, 0);
	
	client_write(self, c, pb->data, pb->packet_len);
}

static void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c, *cnext;
	
	/*
	// debug dump
	if (self->id == 0) {
		hlog(LOG_DEBUG, "o: %*s", pb->packet_len-2, pb->data);
		hlog(LOG_DEBUG, "b:%s%s",
			(pb->flags & F_FROM_UPSTR) ? " from_upstr" : "",
			(pb->flags & F_FROM_DOWNSTR) ? " from_downstr" : ""
			);
	}
	*/
	
	/* specific tight loops */
	
	if (pb->flags & F_DUPE) {
		/* Duplicate packet. Don't send, unless client especially wants! */
		for (c = self->clients_dupe; (c); c = cnext) {
			cnext = c->class_next; // client_write() MAY destroy the client object!
			if (c->state == CSTATE_CONNECTED)
				send_single(self, c, pb);
		}
		
		return;
	}
	
	for (c = self->clients_other; (c); c = cnext) {
		cnext = c->class_next; // client_write() MAY destroy the client object!
		
		/* Do not send to clients that are not logged in. */
		if (c->state != CSTATE_CONNECTED && c->state != CSTATE_COREPEER) {
			//hlog(LOG_DEBUG, "%d/%s: not sending to client: not connected", c->fd, c->username);
			continue;
		}
		
		if (c->flags & CLFLAGS_INPORT) {
			/* Downstream client? If not full feed, process filters
			 * to see if the packet should be sent.
			 */
			if (( (c->flags & CLFLAGS_FULLFEED) != CLFLAGS_FULLFEED) && filter_process(self, c, pb) < 1) {
				//hlog(LOG_DEBUG, "fd %d: Not fullfeed or not matching filter, not sending.", c->fd);
				continue;
			}
		} else if (c->state == CSTATE_COREPEER || (c->flags & CLFLAGS_UPLINKPORT)) {
			/* core peer or uplink? Check that the packet is
			 * coming from a downstream client.
			 */
			if ((pb->flags & F_FROM_DOWNSTR) != F_FROM_DOWNSTR) {
				//hlog(LOG_DEBUG, "fd %d: Not from downstr, not sending to upstr.", c->fd);
				continue;
			}
		} else {
			hlog(LOG_DEBUG, "fd %d: Odd! Client not upstream or downstream. Not sending packets.", c->fd);
			continue;
		}
		
		/* Do not send to read-only sockets */
		if (c->flags & CLFLAGS_PORT_RO) {
			//hlog(LOG_DEBUG, "%d/%s: not sending to client: read-only socket", c->fd, c->username);
			continue;
		}
		
		/* Do not send packet back to the source client.
		   This may reject a packet that came from a socket that got
		   closed a few milliseconds ago and its client_t got
		   recycled on a newly connected client, but if the new client
		   is a long living one, all further packets will be accepted
		   just fine.
		   For packet history dumps this test shall be ignored!
		   Very unlikely check, so check for this last.
		 */
		if (c == pb->origin) {
			//hlog(LOG_DEBUG, "%d: not sending to client: originated from this socketsocket", c->fd);
			continue;
		}
		
		send_single(self, c, pb);
	}
}


/*
 *	Process outgoing packets from the global packet queue, write them to clients
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
		//__sync_synchronize();
		/* Some safety checks against bugs and overload conditions */
		if (pb->is_free) {
			hlog(LOG_ERR, "worker %d: process_outgoing got pbuf %d marked free, age %d (now %d t %d)\n%.*s",
				self->id, pb->seqnum, tick - pb->t, tick, pb->t, pb->packet_len-2, pb->data);
			abort(); /* this would be pretty bad, so we crash immediately */
		} else if (pb->t > tick + 2) {
			/* 2-second offset is normal in case of one thread updating tick earlier than another
			 * and a little thread scheduling luck
			 */
			hlog(LOG_ERR, "worker %d: process_outgoing got packet %d from future with t %d > tick %d!\n%.*s",
				self->id, pb->seqnum, pb->t, tick, pb->packet_len-2, pb->data);
		} else if (tick - pb->t > 5) {
			/* this is a bit too old, are we stuck? */
			hlog(LOG_ERR, "worker %d: process_outgoing got packet %d aged %d sec (now %d t %d)\n%.*s",
				self->id, pb->seqnum, tick - pb->t, tick, pb->t, pb->packet_len-2, pb->data);
		} else {
			process_outgoing_single(self, pb);
		}
		self->last_pbuf_seqnum = pb->seqnum;
		self->pbuf_global_prevp = &pb->next;
	}
	
	while ((pb = *self->pbuf_global_dupe_prevp)) {
		if (pb->is_free) {
			hlog(LOG_ERR, "worker %d: process_outgoing got dupe %d marked free, age %d (now %d t %d)\n%.*s",
				self->id, pb->seqnum, tick - pb->t, tick, pb->t, pb->packet_len-2, pb->data);
			abort();
		} else if (pb->t > tick + 2) {
			hlog(LOG_ERR, "worker: process_outgoing got dupe from future %d with t %d > tick %d!\n%.*s",
				pb->seqnum, pb->t, tick, pb->packet_len-2, pb->data);
		} else if (tick - pb->t > 5) {
			hlog(LOG_ERR, "worker: process_outgoing got dupe %d aged %d sec\n%.*s",
				pb->seqnum, tick - pb->t, pb->packet_len-2, pb->data);
		} else {
			process_outgoing_single(self, pb);
		}
		self->last_pbuf_dupe_seqnum = pb->seqnum;
		self->pbuf_global_dupe_prevp = &pb->next;
	}
	
	if ((e = rwl_rdunlock(&pbuf_global_rwlock))) {
		hlog(LOG_CRIT, "worker: Failed to rdunlock pbuf_global_rwlock!");
		exit(1);
	}
}

