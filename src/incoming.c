
/*
 *	incoming.c: processes incoming data within the worker thread
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "incoming.h"
#include "hmalloc.h"
#include "hlog.h"
#include "parse_aprs.h"

/*
 *	Get a buffer for a packet
 */

struct pbuf_t *pbuf_get_real(struct pbuf_t **pool, struct pbuf_t **global_pool,
	pthread_mutex_t *global_mutex, int len, int bunchlen)
{
	struct pbuf_t *pb;
	struct pbuf_t *last;
	int i;
	
	if (*pool) {
		/* fine, just get the first buffer from the pool...
		 * the pool is not doubly linked (not necessary)
		 */
		pb = *pool;
		*pool = pb->next;
		return pb;
	}
	
	/* The local list is empty... get buffers from the global list. */
	i = 0;
	if (*global_pool) {
		pthread_mutex_lock(global_mutex);
		last = *global_pool;
		
		/* find the last buffer to get from the global pool */
		while (i < bunchlen && (last->next)) {
			last = last->next;
			i++;
		}
		
		/* grab the bunch */
		*pool = *global_pool;
		*global_pool = last->next;
		last->next = NULL;
		
		pthread_mutex_unlock(global_mutex);
	}
	
	hlog(LOG_DEBUG, "pbuf_get_real(%d): got %d bufs from global pool - allocating %d more",
		len, i, bunchlen);
	
	if (i < bunchlen) {
		/* We got too few buffers... allocate a new bunch in the thread-local pool.
		 * We allocate a complete new set of PBUF_ALLOCATE_BUNCH to avoid frequent
		 * grabbing of just a couple of buffers from the global pool.
		 */
		i = 0;
		while (i < bunchlen) {
			pb = hmalloc(sizeof(*pb));
			pb->buf_len = len;
			pb->data = hmalloc(len);
			pb->next = *pool;
			*pool = pb;
			i++;
		}
	}
	
	/* ok, return the first buffer from the pool */
	pb = *pool;
	*pool = pb->next;
	
	/* zero some fields */
	pb->next = NULL;
	pb->flags = 0;
	pb->packettype = 0;
	pb->t = 0;
	pb->packet_len = 0;
	pb->srccall_end = NULL;
	pb->dstcall_end = NULL;
	pb->info_start = NULL;
	pb->lat = pb->lng = 0;
	
	return pb;
}

struct pbuf_t *pbuf_get(struct worker_t *self, int len)
{
	/* select which thread-local freelist to use */
	if (len <= PACKETLEN_MAX_SMALL) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating small buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_small, &pbuf_free_small, &pbuf_free_small_mutex,
			PACKETLEN_MAX_SMALL, PBUF_ALLOCATE_BUNCH_SMALL);
	} else if (len <= PACKETLEN_MAX_LARGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating large buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_large, &pbuf_free_large, &pbuf_free_large_mutex,
			PACKETLEN_MAX_LARGE, PBUF_ALLOCATE_BUNCH_LARGE);
	} else if (len <= PACKETLEN_MAX_HUGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating huge buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_huge, &pbuf_free_huge, &pbuf_free_huge_mutex,
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

int incoming_parse(struct worker_t *self, struct pbuf_t *pb)
{
	char *src_end; /* pointer to the > after srccall */
	char *path_start; /* pointer to the start of the path */
	char *path_end; /* pointer to the : after the path */
	char *packet_end; /* pointer to the end of the packet */
	char *info_start; /* pointer to the beginning of the info */
	char *info_end; /* end of the info */
	char *dstcall_end; /* end of dstcall ([:,]) */
	
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
	
	/* look up end of dstcall */
	/* the logic behind this is slightly uncertain. */
	dstcall_end = path_start;
	while (dstcall_end < path_end && *dstcall_end != ',' && *dstcall_end != ',' && *dstcall_end != '-')
		dstcall_end++;
	
	if (dstcall_end - path_start > CALLSIGNLEN_MAX)
		return -1; /* too long destination callsign */
	
	/* fill necessary info for parsing and dupe checking in the packet buffer */
	pb->srccall_end = src_end;
	pb->dstcall_end = dstcall_end;
	pb->info_start = info_start;
	
	/* just try APRS parsing */
	return parse_aprs(self, pb);
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
	
	/* fill the buffer (it's zeroed by pbuf_get) */
	pb->t = now;
	
	pb->packet_len = len+2;
	memcpy(pb->data, s, len);
	memcpy(pb->data + len, "\r\n", 2); /* append missing CRLF */
	
	/* store the source reference */
	pb->origin = c;
	
	/* do some parsing */
	e = incoming_parse(self, pb);
	if (e < 0) {
		/* failed parsing */
		fprintf(stderr, "Failed parsing (%d):\n", e);
		fwrite(pb->data, len, 1, stderr);
		fprintf(stderr, "\n");
		
		// TODO: return buffer pbuf_put(self, pb);
		return 0;
	}
	
	/* put the buffer in the thread's incoming queue */
	pb->next = NULL;
	*self->pbuf_incoming_local_last = pb;
	self->pbuf_incoming_local_last = &pb->next;
	
	return 0;
}

