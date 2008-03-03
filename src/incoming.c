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
 *	incoming.c: processes incoming data within the worker thread
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <alloca.h>

#include "incoming.h"
#include "hlog.h"
#include "parse_aprs.h"

#include "cellmalloc.h"

/* global packet buffer freelists */

cellarena_t *pbuf_cells_small;
cellarena_t *pbuf_cells_large;
cellarena_t *pbuf_cells_huge;


/*
 *	Get a buffer for a packet
 *
 *	pbuf_t buffers are accumulated into each worker local buffer in small sets,
 *	and then used from there.  The buffers are returned into global pools.
 */

void pbuf_init(void)
{
	pbuf_cells_small = cellinit(sizeof(struct pbuf_t) + PACKETLEN_MAX_SMALL,
				    __alignof__(struct pbuf_t), 0 /* FIFO! */,
				    256 /* 256 kB at the time */);
	pbuf_cells_large = cellinit(sizeof(struct pbuf_t) + PACKETLEN_MAX_LARGE,
				    __alignof__(struct pbuf_t), 0 /* FIFO! */,
				    256 /* 256 kB at the time */);
	pbuf_cells_huge  = cellinit(sizeof(struct pbuf_t) + PACKETLEN_MAX_HUGE,
				    __alignof__(struct pbuf_t), 0 /* FIFO! */,
				    256 /* 256 kB at the time */);
}

/*
 *	pbuf_free  sends buffer back to worker local pool, or when invoked
 *	without 'self' pointer, like in final history buffer cleanup,
 *	to the global pool.
 */

void pbuf_free(struct worker_t *self, struct pbuf_t *p)
{
	if (self) { /* Return to worker local pool */
		switch (p->buf_len) {
		case PACKETLEN_MAX_SMALL:
			p->next = self->pbuf_free_small;
			self->pbuf_free_small = p;
			return;
		case PACKETLEN_MAX_LARGE:
			p->next = self->pbuf_free_large;
			self->pbuf_free_large = p;
			return;
		case PACKETLEN_MAX_HUGE:
			p->next = self->pbuf_free_huge;
			self->pbuf_free_huge = p;
			return;
		default:
			break;
		}
	}

	/* Not worker local processing then, return to global pools. */

	switch (p->buf_len) {
	case PACKETLEN_MAX_SMALL:
		cellfree(pbuf_cells_small, p);
		break;
	case PACKETLEN_MAX_LARGE:
		cellfree(pbuf_cells_large, p);
		break;
	case PACKETLEN_MAX_HUGE:
		cellfree(pbuf_cells_huge, p);
		break;
	default:
		hlog(LOG_ERR, "pbuf_free(%p) - packet length not known: %d", p, p->buf_len);
		break;
	}
}

/*
 *	pbuf_free_many  sends buffers back to the global pool in groups
 *                      after size sorting them...  
 *			Multiple cells are returned with single mutex.
 */

void pbuf_free_many(struct pbuf_t **array, int numbufs)
{
	void **arraysmall  = alloca(sizeof(void*)*numbufs);
	void **arraylarge  = alloca(sizeof(void*)*numbufs);
	void **arrayhuge   = alloca(sizeof(void*)*numbufs);
	int i;
	int smallcnt = 0, largecnt = 0, hugecnt = 0;

	for (i = 0; i < numbufs; ++i) {
		switch (array[i]->buf_len) {
		case PACKETLEN_MAX_SMALL:
			arraysmall[smallcnt++] = array[i];
			break;
		case PACKETLEN_MAX_LARGE:
			arraylarge[largecnt++] = array[i];
			break;
		case PACKETLEN_MAX_HUGE:
			arrayhuge[hugecnt++]   = array[i];
			break;
		default:
			hlog(LOG_ERR, "pbuf_free(%p) - packet length not known: %d", array[i], array[i]->buf_len);
			break;
		}
	}
	if (smallcnt > 0)
		cellfreemany(pbuf_cells_small, arraysmall, smallcnt);
	if (largecnt > 0)
		cellfreemany(pbuf_cells_large, arraylarge, largecnt);
	if (hugecnt > 0)
		cellfreemany(pbuf_cells_huge,  arrayhuge,   hugecnt);
}

struct pbuf_t *pbuf_get_real(struct pbuf_t **pool, cellarena_t *global_pool,
			     int len, int bunchlen)
{
	struct pbuf_t *pb;
	int i;
	struct pbuf_t **allocarray = alloca(bunchlen * sizeof(void*));
	
	if (*pool) {
		/* fine, just get the first buffer from the pool...
		 * the pool is not doubly linked (not necessary)
		 */
		pb = *pool;
		*pool = pb->next;
		return pb;
	}
	
	/* The local list is empty... get buffers from the global list. */

	bunchlen = cellmallocmany( global_pool, (void**)allocarray, bunchlen );

	for ( i = 0;  i < bunchlen; ++i ) {
		if (i > 0)
		  (*pool)->next = allocarray[i];
		*pool = allocarray[i];
	}
	if (*pool)
		(*pool)->next = NULL;

	hlog(LOG_DEBUG, "pbuf_get_real(%d): got %d bufs from global pool",
	     len, bunchlen);
	
	
	/* ok, return the first buffer from the pool */
	pb = *pool;
	if (!pb) return NULL;
	*pool = pb->next;

	/* zero all header fields */
	memset(pb, 0, sizeof(*pb));

	/* we know the length in this sub-pool, set it */
	pb->buf_len = len;
	
	return pb;
}

struct pbuf_t *pbuf_get(struct worker_t *self, int len)
{
	/* select which thread-local freelist to use */
	if (len <= PACKETLEN_MAX_SMALL) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating small buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_small, pbuf_cells_small,
				     PACKETLEN_MAX_SMALL, PBUF_ALLOCATE_BUNCH_SMALL);
	} else if (len <= PACKETLEN_MAX_LARGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating large buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_large, pbuf_cells_large,
				     PACKETLEN_MAX_LARGE, PBUF_ALLOCATE_BUNCH_LARGE);
	} else if (len <= PACKETLEN_MAX_HUGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating huge buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_huge, pbuf_cells_huge,
				     PACKETLEN_MAX_HUGE, PBUF_ALLOCATE_BUNCH_HUGE);
	} else { /* too large! */
		hlog(LOG_ERR, "pbuf_get: Not allocating a buffer for a packet of %d bytes!", len);
		return NULL;
	}
}

/*
 *	Move incoming packets from the thread-local incoming buffer
 *	(self->pbuf_incoming_local) to self->incoming local for the
 *	dupecheck thread to catch them
 */

void incoming_flush(struct worker_t *self)
{
	/* try grab the lock.. if it fails, we'll try again, either
	 * in 200 milliseconds or after next input
	 */
	if (pthread_mutex_trylock(&self->pbuf_incoming_mutex) != 0)
		return;
		
	*self->pbuf_incoming_last = self->pbuf_incoming_local;
	pthread_mutex_unlock(&self->pbuf_incoming_mutex);
	
	/* clean the local lockfree queue */
	self->pbuf_incoming_local = NULL;
	self->pbuf_incoming_local_last = &self->pbuf_incoming_local;
}

/*
 *	Parse an incoming packet
 */

int incoming_parse(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	char *src_end; /* pointer to the > after srccall */
	char *path_start; /* pointer to the start of the path */
	char *path_end; /* pointer to the : after the path */
	char *packet_end; /* pointer to the end of the packet */
	char *info_start; /* pointer to the beginning of the info */
	char *info_end; /* end of the info */
	char *dstcall_end; /* end of dstcall ([:,]) */
	int rc;
	
	/* a packet looks like:
	 * SRCCALL>DSTCALL,PATH,PATH:INFO\r\n
	 * (we have normalized the \r\n by now)
	 */
	
	packet_end = pb->data + pb->packet_len; /* for easier overflow checking */
	
	/* look for the '>' */
	src_end = memchr(pb->data, '>', pb->packet_len < CALLSIGNLEN_MAX+1 ? pb->packet_len : CALLSIGNLEN_MAX+1);
	if (!src_end)
		return -1;
	
	path_start = src_end+1;
	if (path_start >= packet_end)
		return -1;
	
	if (src_end - pb->data > CALLSIGNLEN_MAX)
		return -1; /* too long source callsign */
	
	/* look for the : */
	path_end = memchr(path_start, ':', packet_end - path_start);
	if (!path_end)
		return -1;
	
	info_start = path_end+1;
	if (info_start >= packet_end)
		return -1;
	
	/* see that there is at least some data in the packet */
	info_end = packet_end - 2;
	if (info_end <= info_start)
		return -1;
	
	/* look up end of dstcall (excluding SSID - this is the way dupecheck and
	 * mic-e parser wants it)
	 */
	dstcall_end = path_start;
	while (dstcall_end < path_end && *dstcall_end != '-' && *dstcall_end != ',' && *dstcall_end != ':')
		dstcall_end++;
	
	if (dstcall_end - path_start > CALLSIGNLEN_MAX)
		return -1; /* too long destination callsign */
	
	/* fill necessary info for parsing and dupe checking in the packet buffer */
	pb->srccall_end = src_end;
	pb->dstcall_end = dstcall_end;
	pb->info_start = info_start;
	
	/* just try APRS parsing */
	rc = parse_aprs(self, pb);
	if ((pb->packettype & (T_POSITION|T_OBJECT)) && // FIXME: all packets with position data..
	    memcmp(pb->data, c->username,  src_end - pb->data) == 0) {
		// FIXME: If packet source call matches client login callsign,
		// fill in  c->my_lat/my_lon/my_coslat
		// NOTE: ITEMs are usually for other identities than their sender..
	}
	return rc;
}

/*
 *	Handler called by the socket reading function for normal APRS-IS traffic
 */

int incoming_handler(struct worker_t *self, struct client_t *c, char *s, int len)
{
	struct pbuf_t *pb;
	int e;
	
	/* note: len does not include CRLF, it's reconstructed here... we accept
	 * CR, LF or CRLF on input but make sure to use CRLF on output.
	 */
	
	/* make sure it looks somewhat like an APRS-IS packet */
	if (len < PACKETLEN_MIN || len+2 > PACKETLEN_MAX) {
		hlog(LOG_WARNING, "Packet size out of bounds (%d): %s", len, s);
		return 0;
	}
	
	 /* starts with # => a comment packet, timestamp or something */
	if (*s == '#')
		return 0;

	/* get a packet buffer */
	if (!(pb = pbuf_get(self, len+2)))
		return 0;
	
	/* store the source reference */
	pb->origin = c;
	
	/* when it was received ? */
	pb->t = now;
	
	/* How much there really is data ? */
	pb->packet_len = len+2;

	/* Actual data */
	memcpy(pb->data, s, len);
	memcpy(pb->data + len, "\r\n", 2); /* append missing CRLF */

	/* do some parsing */
	e = incoming_parse(self, c, pb);
	if (e < 0) {
		/* failed parsing */
		fprintf(stderr, "Failed parsing (%d):\n", e);
		fwrite(pb->data, len, 1, stderr);
		fprintf(stderr, "\n");
		
		// So it failed, do send it out anyway....
		// pbuf_free(self, pb);
		// return 0;
		// FIXME: if it's COMPLETELY garbled, ie. not SRC>DST:DATA, do not send it out
		// - successful APRS parsing is not required.
	}
	
	/* put the buffer in the thread's incoming queue */
	pb->next = NULL;
	*self->pbuf_incoming_local_last = pb;
	self->pbuf_incoming_local_last = &pb->next;
	
	return 0;
}

