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
#include "status.h"

/*
 *	send a single packet to all clients (and peers and uplinks) which
 *	should have a copy
 */

static inline void send_single(struct worker_t *self, struct client_t *c, char *data, int len)
{
	/* if we're going to use the UDP sidechannel, account for UDP, otherwise
	 * its TCP or SCTP or something.
	 */
	if (c->udp_port && c->udpclient)
		clientaccount_add( c, IPPROTO_UDP, 0, 0, 0, 1, 0, 0);
	else
		clientaccount_add( c, c->ai_protocol, 0, 0, 0, 1, 0, 0);
	
	c->write(self, c, data, len);
}

static void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c, *cnext;
	struct client_t *origin = pb->origin; /* reduce pointer deferencing in tight loops */
	
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
		if (self->clients_dupe) {
			/* if we have any dupe clients at all, generate a version with "dup\t"
			 * prefix to avoid regular clients processing dupes
			 */
			char dupe_sendbuf[PACKETLEN_MAX+7];
			memcpy(dupe_sendbuf, "dup\t", 4);
			memcpy(dupe_sendbuf + 4, pb->data, pb->packet_len);
			int dupe_len = pb->packet_len + 4;
			
			for (c = self->clients_dupe; (c); c = cnext) {
				cnext = c->class_next; // client_write() MAY destroy the client object!
				send_single(self, c, dupe_sendbuf, dupe_len);
			}
		}
		
		/* Check if I have the client which sent this dupe, and
		 * increment it's dupe counter
		 */
		/* OPTIMIZE: we walk through all clients for each dupe - how to find it quickly? */
		for (c = self->clients; (c); c = c->next) {
			if (c == origin) {
				clientaccount_add(c, -1, 0, 0, 0, 0, 0, 1);
				break;
			}
		}
		
		return;
	}

	if (pb->flags & F_FROM_DOWNSTR) {
		/* client is from downstream, send to upstreams and peers */
		for (c = self->clients_ups; (c); c = cnext) {
			cnext = c->class_next; // client_write() MAY destroy the client object!
			if (c != origin)
				send_single(self, c, pb->data, pb->packet_len);
		}
	}
	
	/* packet came from anywhere and is not a dupe - let's go through the
	 * clients who connected us
	 */
	for (c = self->clients_other; (c); c = cnext) {
		cnext = c->class_next; // client_write() MAY destroy the client object!
		
		/* If not full feed, process filters to see if the packet should be sent. */
		if (( (c->flags & CLFLAGS_FULLFEED) != CLFLAGS_FULLFEED) && filter_process(self, c, pb) < 1) {
			//hlog(LOG_DEBUG, "fd %d: Not fullfeed or not matching filter, not sending.", c->fd);
			continue;
		}
		
		/* Do not send packet back to the source client.
		   This may reject a packet that came from a socket that got
		   closed a few milliseconds ago and its client_t got
		   recycled on a newly connected client, but if the new client
		   is a long living one, all further packets will be accepted
		   just fine.
		   Very unlikely check, so check for this last.
		 */
		if (c == origin) {
			//hlog(LOG_DEBUG, "%d: not sending to client: originated from this socketsocket", c->fd);
			continue;
		}
		
		send_single(self, c, pb->data, pb->packet_len);
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
			self->internal_packet_drops++;
			if (self->internal_packet_drops > 10)
				status_error(86400, "packet_drop_future");
		} else if (tick - pb->t > 5) {
			/* this is a bit too old, are we stuck? */
			hlog(LOG_ERR, "worker %d: process_outgoing got packet %d aged %d sec (now %d t %d)\n%.*s",
				self->id, pb->seqnum, tick - pb->t, tick, pb->t, pb->packet_len-2, pb->data);
			self->internal_packet_drops++;
			if (self->internal_packet_drops > 10)
				status_error(86400, "packet_drop_hang");
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

