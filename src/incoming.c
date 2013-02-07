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
 *	incoming.c: processes incoming data within the worker thread
 */

#include "ac-hdrs.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

#include <stdlib.h>

#include "config.h"
#include "incoming.h"
#include "hlog.h"
#include "parse_aprs.h"
#include "parse_qc.h"
#include "filter.h"
#include "clientlist.h"
#include "client_heard.h"
#include "version.h"
#include "cellmalloc.h"
#include "messaging.h"
#include "dupecheck.h"

/* When adding labels here, remember to add the description strings in
 * web/aprsc.js rx_err_strings
 */
const char *inerr_labels[] = {
	"unknown",
	"no_colon",
	"no_dst",
	"no_path",
	"inv_srccall",
	"no_body",
	"inv_dstcall",
	"disallow_unverified",
	"disallow_unverified_path",
	"path_nogate",
	"party_3rd_ip", /* was 3rd_party, but labels starting with numbers collide with munin */
	"party_3rd_inv",
	"general_query",
	"aprsc_oom_pbuf",
	"aprsc_class_fail",
	"aprsc_q_bug",
	"q_drop",
	"short_packet",
	"long_packet",
	"inv_path_call",
	"q_qax",
	"q_qaz",
	"q_path_mycall",
	"q_path_call_twice",
	"q_path_login_not_last",
	"q_path_call_is_local",
	"q_path_call_inv",
	"q_qau_path_call_srccall",
	"q_newq_buffer_small",
	"q_nonval_multi_q_calls",
	"q_i_no_viacall"
};

#define incoming_strerror(i) ((i <= 0 && i >= INERR_MIN) ? inerr_labels[i * -1] : inerr_labels[0])

#ifdef _FOR_VALGRIND_
typedef struct cellarena_t {
  int dummy;
} cellarena_t;
#endif
/* global packet buffer freelists */

cellarena_t *pbuf_cells_small;
cellarena_t *pbuf_cells_medium;
cellarena_t *pbuf_cells_large;

int pbuf_cells_kb = 2048; /* 2M bunches is faster for system than 16M ! */

/*
 *	Get a buffer for a packet
 *
 *	pbuf_t buffers are accumulated into each worker local buffer in small sets,
 *	and then used from there.  The buffers are returned into global pools.
 */

void pbuf_init(void)
{
#ifndef _FOR_VALGRIND_
	pbuf_cells_small  = cellinit( "pbuf small",
				      sizeof(struct pbuf_t) + PACKETLEN_MAX_SMALL,
				      __alignof__(struct pbuf_t), CELLMALLOC_POLICY_FIFO,
				      pbuf_cells_kb /* n kB at the time */, 0 /* minfree */ );
	pbuf_cells_medium = cellinit( "pbuf medium",
				      sizeof(struct pbuf_t) + PACKETLEN_MAX_MEDIUM,
				      __alignof__(struct pbuf_t), CELLMALLOC_POLICY_FIFO,
				      pbuf_cells_kb /* n kB at the time */, 0 /* minfree */ );
	pbuf_cells_large  = cellinit( "pbuf large",
				      sizeof(struct pbuf_t) + PACKETLEN_MAX_LARGE,
				      __alignof__(struct pbuf_t), CELLMALLOC_POLICY_FIFO,
				      pbuf_cells_kb /* n kB at the time */, 0 /* minfree */ );
#endif
}

/*
 *	pbuf_free  sends buffer back to worker local pool, or when invoked
 *	without 'self' pointer, like in final history buffer cleanup,
 *	to the global pool.
 */

void pbuf_free(struct worker_t *self, struct pbuf_t *p)
{
	if (self) { /* Return to worker local pool */

		// hlog(LOG_DEBUG, "pbuf_free(%p) for worker %p - packet length: %d", p, self, p->buf_len);

		switch (p->buf_len) {
		case PACKETLEN_MAX_SMALL:
			p->next = self->pbuf_free_small;
			self->pbuf_free_small = p;
			break;
		case PACKETLEN_MAX_MEDIUM:
			p->next = self->pbuf_free_medium;
			self->pbuf_free_medium = p;
			break;
		case PACKETLEN_MAX_LARGE:
			p->next = self->pbuf_free_large;
			self->pbuf_free_large = p;
			break;
		default:
			hlog(LOG_ERR, "pbuf_free(%p) for worker %p - packet length not known: %d", p, self, p->buf_len);
			break;
		}
		return;
	}

#ifndef _FOR_VALGRIND_

	/* Not worker local processing then, return to global pools. */

	// hlog(LOG_DEBUG, "pbuf_free(%p) for global pool - packet length: %d", p, p->buf_len);

	switch (p->buf_len) {
	case PACKETLEN_MAX_SMALL:
		cellfree(pbuf_cells_small, p);
		break;
	case PACKETLEN_MAX_MEDIUM:
		cellfree(pbuf_cells_medium, p);
		break;
	case PACKETLEN_MAX_LARGE:
		cellfree(pbuf_cells_large, p);
		break;
	default:
		hlog(LOG_ERR, "pbuf_free(%p) - packet length not known: %d", p, p->buf_len);
		break;
	}
	return;
#else
	hfree(p);
	return;
#endif
}

/*
 *	pbuf_free_many  sends buffers back to the global pool in groups
 *                      after size sorting them...  
 *			Multiple cells are returned with single mutex.
 */

void pbuf_free_many(struct pbuf_t **array, int numbufs)
{
	int i;
	void **arraysmall   = alloca(sizeof(void*)*numbufs);
	void **arraymedium  = alloca(sizeof(void*)*numbufs);
	void **arraylarge   = alloca(sizeof(void*)*numbufs);
	int smallcnt = 0, mediumcnt = 0, largecnt = 0;

	for (i = 0; i < numbufs; ++i) {
		array[i]->is_free = 1;
		//__sync_synchronize();
		switch (array[i]->buf_len) {
		case PACKETLEN_MAX_SMALL:
			arraysmall [smallcnt++]  = array[i];
			break;
		case PACKETLEN_MAX_MEDIUM:
			arraymedium[mediumcnt++] = array[i];
			break;
		case PACKETLEN_MAX_LARGE:
			arraylarge [largecnt++]  = array[i];
			break;
		default:
		  hlog( LOG_ERR, "pbuf_free_many(%p) - packet length not known: %d :%d",
			array[i], array[i]->buf_len, array[i]->packet_len );
			break;
		}
	}

	// hlog( LOG_DEBUG, "pbuf_free_many(); counts: small %d large %d huge %d", smallcnt, mediumcnt, largecnt );

#ifndef _FOR_VALGRIND_
	if (smallcnt > 0)
		cellfreemany(pbuf_cells_small,  arraysmall,  smallcnt);
	if (mediumcnt > 0)
		cellfreemany(pbuf_cells_medium, arraymedium, mediumcnt);
	if (largecnt > 0)
		cellfreemany(pbuf_cells_large,  arraylarge,  largecnt);

#else
	for (i = 0; i < numbufs; ++i) {
		hfree(array[i]);
	}
#endif
}

/*
 *	pbuf_dump_*: tools to dump packet buffers to a file
 */

static void pbuf_dump_entry(FILE *fp, struct pbuf_t *pb)
{
	fprintf(fp, "%ld\t",	(long)pb->t); /* arrival time */
	fprintf(fp, "%x\t",	pb->packettype);
	fprintf(fp, "%x\t",	pb->flags);
      	fprintf(fp, "%.*s\t",	pb->srcname_len, pb->srcname);
	fprintf(fp, "%f\t%f\t",	pb->lat, pb->lng);
	fprintf(fp, "%d\t",     pb->packet_len);
	fwrite(pb->data, pb->packet_len-2, 1, fp); /* without terminating CRLF */
	fprintf(fp, "\n");
}

void pbuf_dump(FILE *fp)
{
	/* Dump the pbuf queue out on text format */
	struct pbuf_t *pb = pbuf_global;
	
	for ( ; pb ; pb = pb->next ) {
		pbuf_dump_entry(fp, pb);
	}
}

void pbuf_dupe_dump(FILE *fp)
{
	/* Dump the pbuf queue out on text format */
	struct pbuf_t *pb = pbuf_global_dupe;
	
	for ( ; pb ; pb = pb->next ) {
		pbuf_dump_entry(fp, pb);
	}
}

/*
 *	get a buffer for an incoming packet, from either a thread-local
 *	freelist of preallocated buffers, or from the global cellmalloc
 *	area.
 */

static struct pbuf_t *pbuf_get(struct worker_t *self, int len)
{
	struct pbuf_t *pb;
	int i;
	struct pbuf_t **allocarray;
	struct pbuf_t **pool;
	cellarena_t *global_pool;
	int bunchlen;

	/* select which thread-local freelist to use */
	if (len <= PACKETLEN_MAX_SMALL) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating small buffer for a packet of %d bytes", len);
		pool        = &self->pbuf_free_small;
		global_pool = pbuf_cells_small;
		len         = PACKETLEN_MAX_SMALL;
		bunchlen    = PBUF_ALLOCATE_BUNCH_SMALL;
	} else if (len <= PACKETLEN_MAX_MEDIUM) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating large buffer for a packet of %d bytes", len);
		pool        = &self->pbuf_free_medium;
		global_pool = pbuf_cells_medium;
		len         = PACKETLEN_MAX_MEDIUM;
		bunchlen    = PBUF_ALLOCATE_BUNCH_MEDIUM;
	} else if (len <= PACKETLEN_MAX_LARGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating huge buffer for a packet of %d bytes", len);
		pool        = &self->pbuf_free_large;
		global_pool = pbuf_cells_large;
		len         = PACKETLEN_MAX_LARGE;
		bunchlen    = PBUF_ALLOCATE_BUNCH_LARGE;
	} else { /* too large! */
		hlog(LOG_ERR, "pbuf_get: Not allocating a buffer for a packet of %d bytes!", len);
		return NULL;
	}

	allocarray = alloca(bunchlen * sizeof(void*));

	if (*pool) {
		/* fine, just get the first buffer from the freelist pool...
		 * the pool is not doubly linked (not necessary)
		 */
		pb = *pool;
		*pool = pb->next;

		/* zero all header fields */
		memset(pb, 0, sizeof(*pb));

		/* we know the length in this sub-pool, set it */
		pb->buf_len = len;

		// hlog(LOG_DEBUG, "pbuf_get(%d): got one buf from local pool: %p", len, pb);

		return pb;
	}
	
#ifndef _FOR_VALGRIND_
	/* The local list is empty... get buffers from the global list. */

	bunchlen = cellmallocmany( global_pool, (void**)allocarray, bunchlen );
	if (bunchlen < 1) {
		hlog(LOG_CRIT, "aprsc: Out of memory: Could not allocate packet buffers!");
		abort();
	}

	for ( i = 1;  i < bunchlen; ++i ) {
		pb = allocarray[i];
		pb->is_free = 0;
		pb->next    = *pool;
		pb->buf_len = len; // this is necessary for worker local pool discard at worker shutdown
		*pool = pb;
	}

	pb = allocarray[0];

	// hlog(LOG_DEBUG, "pbuf_get(%d): got %d bufs from global pool %p", len, bunchlen, pool);

	/* ok, return the first buffer from the pool */

	/* zero all header fields */
	memset(pb, 0, sizeof(*pb));

	/* we know the length in this sub-pool, set it */
	pb->buf_len = len;
	
	return pb;

#else /* Valgrind -version of things */


	/* The local list is empty... get buffers from the global list. */

	int sz = sizeof(struct pbuf_t) + len;

	for ( i = 1;  i < bunchlen; ++i ) {
		pb = hmalloc(sz);
		pb->next = *pool;
		pb->buf_len = len; // for valgrind this is not necessary.. but exists for symmetry's sake
		*pool = pb;
	}

	pb = hmalloc(sz);
	
	memset(pb, 0, sz);
	pb->buf_len = len;

	// hlog(LOG_DEBUG, "pbuf_get_real(%d): got %d bufs to local pool, returning %p", len, bunchlen, pb);

	return pb;
#endif
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
	self->pbuf_incoming_last  = self->pbuf_incoming_local_last;
	self->pbuf_incoming_count += self->pbuf_incoming_local_count;
	pthread_mutex_unlock(&self->pbuf_incoming_mutex);

	//hlog( LOG_DEBUG, "incoming_flush() sent out %d packets", self->pbuf_incoming_local_count );

#ifdef USE_EVENTFD
	/* wake up dupecheck from sleep */
	uint64_t u = 1; // could set to pbuf_incoming_local_count, but it wouldn't be used for anything.
	int i = write(dupecheck_eventfd, &u, sizeof(uint64_t));
	if (i != sizeof(uint64_t)) {
		hlog(LOG_ERR, "incoming_flush() failed to write to dupecheck_eventfd: %s", strerror(errno));
	}
#endif
	
	/* clean the local lockfree queue */
	self->pbuf_incoming_local = NULL;
	self->pbuf_incoming_local_last = &self->pbuf_incoming_local;
	self->pbuf_incoming_local_count = 0;
}

/*
 *	Find a string in a binary buffer
 */

char *memstr(char *needle, char *haystack, char *haystack_end)
{
	char *hp = haystack;
	char *np = needle;
	char *match_start = NULL;
	
	while (hp < haystack_end) {
		if (*hp == *np) {
			/* matching... is this the start of a new match? */
			if (match_start == NULL)
				match_start = hp;
			/* increase needle pointer, so we'll check the next char */
			np++;
		} else {
			/* not matching... clear state */
			match_start = NULL;
			np = needle;
		}
		
		/* if we've reached the end of the needle, and we have found a match,
		 * return a pointer to it
		 */
		if (*np == 0 && (match_start))
			return match_start;
		hp++;
	}
	
	/* out of luck */
	return NULL;
}

/*
 *	Find a string in a binary buffer, case insensitive
 */

char *memcasestr(char *needle, char *haystack, char *haystack_end)
{
	char *hp = haystack;
	char *np = needle;
	char *match_start = NULL;
	
	while (hp < haystack_end) {
		if (toupper(*hp) == toupper(*np)) {
			/* matching... is this the start of a new match? */
			if (match_start == NULL)
				match_start = hp;
			/* increase needle pointer, so we'll check the next char */
			np++;
		} else {
			/* not matching... clear state */
			match_start = NULL;
			np = needle;
		}
		
		/* if we've reached the end of the needle, and we have found a match,
		 * return a pointer to it
		 */
		if (*np == 0 && (match_start))
			return match_start;
		hp++;
	}
	
	/* out of luck */
	return NULL;
}

/*
 *	Check if the digipeater path includes elements indicating that the
 *	packet should be dropped.
 */

static int digi_path_drop(char *via_start, char *path_end)
{
	if (memstr(",NOGATE", via_start, path_end))
		return 1;
		
	if (memstr(",RFONLY", via_start, path_end))
		return 1;
		
	return 0;
}

/*
 *	Check if a callsign is good for srccall/dstcall
 *	(valid APRS-IS callsign, * not allowed)
 */

int check_invalid_src_dst(const char *call, int len)
{
	const char *p = call;
	const char *e = call + len;
	
	//hlog(LOG_DEBUG, "check_invalid_src_dst: '%.*s'", len, call);
	
	if (len < 1 || len > CALLSIGNLEN_MAX)
		return -1;
	
	while (p < e) {
		/* alphanumeric and - */
		if ((!isalnum(*p)) && *p != '-')
			return -1;
			
		p++;
	}
	
	return 0;
}

/*
 *	Check if a callsign is good for a digi path entry
 *	(valid APRS-IS callsign, * allowed in end)
 */

static int check_invalid_path_callsign(const char *call, int len, int after_q)
{
	const char *p = call;
	const char *e = call + len;
	
	//hlog(LOG_DEBUG, "check_invalid_path_callsign: '%.*s'", len, call);
	
	/* only check for minimum length first - max length depends on
	 * if there's a * in the end
	 */
	if (len < 1)
		return -1;
	
	while (p < e) {
		/* alphanumeric and - */
		/* AND '*' is allowed in the end */
		if ((!isalnum(*p)) && *p != '-' && !(*p == '*' && p == e-1)) {
			//hlog(LOG_DEBUG, "check_invalid_path_callsign: invalid char '%c'", *p);
			return -1;
		}
		
		p++;
	}
	
	if (*(p-1) == '*' && len <= CALLSIGNLEN_MAX+1) {
		//hlog(LOG_DEBUG, "check_invalid_path_callsign: allowing len %d due to last *", len);
		return 0;
	}
	
	/* too long? */
	if (len > CALLSIGNLEN_MAX) {
		// TODO: more specific test for IPv6 trace address
		if (after_q && len == 32) {
			//hlog(LOG_DEBUG, "check_invalid_path_callsign: ipv6 address '%.*s'", len, call);
			return 0;
		}
		
		hlog(LOG_DEBUG, "check_invalid_path_callsign: too long len %d", len);
		return -1;
	}
	
	return 0;
}

/*
 *	Check for invalid callsigns in path
 */

int check_path_calls(const char *via_start, const char *path_end)
{
	const char *p = via_start + 1;
	const char *e;
	int calls = 0;
	int after_q = 0;
	
	while (p < path_end) {
		calls++;
		e = p;
		/* find end of path callsign */
		while (*e != ',' && e < path_end)
			e++;
			
		/* is this a q construct? */
		if (*p == 'q' && e-p == 3) {
			//hlog(LOG_DEBUG, "check_path_calls found Q construct: '%.*s'", e-p, p);
			after_q = 1;
			p = e + 1;
			continue;
		}
		
		//hlog(LOG_DEBUG, "check_path_calls: '%.*s'%s", e-p, p, (after_q) ? " after q" : "");
		if (check_invalid_path_callsign(p, e-p, after_q) != 0)
			return -1;
		
		p = e + 1;
	}
	
	//hlog(LOG_DEBUG, "check_path_calls returning %d", calls);
	
	return calls;
}

/*
 *	Handle incoming messages to SERVER
 */

static int incoming_server_message(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	struct aprs_message_t am;
	
	int e;
	if ((e = parse_aprs_message(pb, &am))) {
		hlog(LOG_DEBUG, "message to SERVER from %.*s failed message parsing: %d", pb->srcname_len, pb->srcname, e);
		return 0;
	}
	
	if (am.is_ack) {
		hlog(LOG_DEBUG, "message ack to SERVER from %.*s for msgid '%.*s'", pb->srcname_len, pb->srcname, am.msgid_len, am.msgid);
		return 0;
	}
	
	hlog(LOG_DEBUG, "message to SERVER from %.*s: '%.*s'", pb->srcname_len, pb->srcname, am.body_len, am.body);
	
	/* send ack */
	if (am.msgid) {
		if ((e = messaging_ack(self, c, pb, &am)) < 0) {
			hlog(LOG_DEBUG, "failed to ack message to SERVER from %.*s: '%.*s': %d",
				pb->srcname_len, pb->srcname, am.body_len, am.body, e);
			return e;
		}
	}
	
	if (strncasecmp(am.body, "filter?", 7) == 0)
		return messaging_message_client(self, c, "filter %s active", c->filter_s);
	
	if (strncasecmp(am.body, "filter", 6) == 0)
		return filter_commands(self, c, 1, am.body, am.body_len);
	
	/* unknown command */
	return messaging_message_client(self, c, "Unknown command");
}

/*
 *	Parse an incoming packet.
 *
 *	Returns -1 if the packet is pathologically invalid on APRS-IS
 *	and can be discarded, 0 if it is correct for APRS-IS and will be
 *	forwarded, 1 if it was successfully parsed by the APRS parser.
 *
 *	This function also allocates the pbuf structure for the new packet
 *	and forwards it to the dupecheck thread.
 */

// Must be large enough to accommodate the largest packet we accept on input
// + the length of the IPv6 qAI appended address and our callsign
#define PATH_APPEND_LEN 600

int incoming_parse(struct worker_t *self, struct client_t *c, char *s, int len)
{
	struct pbuf_t *pb;
	char *src_end; /* pointer to the > after srccall */
	char *path_start; /* pointer to the start of the path */
	char *path_end; /* pointer to the : after the path */
	const char *packet_end; /* pointer to the end of the packet */
	const char *info_start; /* pointer to the beginning of the info */
	const char *info_end; /* end of the info */
	char *dstcall_end_or_ssid; /* end of dstcall, before SSID ([-:,]) */
	char *dstcall_end; /* end of dstcall including SSID ([:,]) */
	char *via_start; /* start of the digipeater path (after dstcall,) */
	char *q_start = NULL; /* start of the Q construct (points to the 'q') */
	char *q_replace = NULL; /* whether the existing Q construct is replaced */
	char *data;	  /* points to original incoming path/payload separating ':' character */
	int datalen;		  /* length of the data block excluding tail \r\n */
	int pathlen;		  /* length of the path  ==  data-s  */
	int rc;
	char path_append[PATH_APPEND_LEN]; /* data to be appended to the path (generated Q construct, etc), could be long */
	int path_append_len;
	int originated_by_client = 0;
	char *p;
	
	/* for quirky clients, trim spaces and NULs from beginning */
	if (c->quirks_mode) {
		while ((*s == ' ' || *s == 0) && len > 0) {
			len--;
			s++;
		}
	}
	
	/* check for minimum length of packet */
	if (len < PACKETLEN_MIN-2)
		return INERR_SHORT_PACKET;
	
	// Easy pointer for comparing against far end..
	packet_end = s + len;
	
	/* a packet looks like:
	 * SRCCALL>DSTCALL,PATH,PATH:INFO\r\n
	 * (we have normalized the \r\n by now)
	 */

	path_end = memchr(s, ':', len);
	if (!path_end)
		return INERR_NO_COLON; // No ":" in the packet
	pathlen = path_end - s;

	data = path_end;            // Begins with ":"
	datalen = len - pathlen;    // Not including line end \r\n

	/* look for the '>' */
	src_end = memchr(s, '>', pathlen < CALLSIGNLEN_MAX+1 ? pathlen : CALLSIGNLEN_MAX+1);
	if (!src_end)
		return INERR_NO_DST;	// No ">" in packet start..
	
	path_start = src_end+1;
	if (path_start >= packet_end)	// We're already at the path end
		return INERR_NO_PATH;
	
	if (check_invalid_src_dst(s, src_end - s) != 0)
		return INERR_INV_SRCCALL; /* invalid or too long for source callsign */
	
	info_start = path_end+1;	// @":"+1 - first char of the payload
	if (info_start >= packet_end)
		return INERR_NO_BODY;
	
	/* see that there is at least some data in the packet */
	info_end = packet_end;
	if (info_end <= info_start)
		return INERR_NO_BODY;
	
	/* look up end of dstcall (excluding SSID - this is the way dupecheck and
	 * mic-e parser wants it)
	 */

	dstcall_end = path_start;
	while (dstcall_end < path_end && *dstcall_end != '-' && *dstcall_end != ',' && *dstcall_end != ':')
		dstcall_end++;
	dstcall_end_or_ssid = dstcall_end; // OK, SSID is here (or the dstcall end), go for the real end
	while (dstcall_end < path_end && *dstcall_end != ',' && *dstcall_end != ':')
		dstcall_end++;
	
	if (check_invalid_src_dst(path_start, dstcall_end - path_start))
		return INERR_INV_DSTCALL; /* invalid or too long for destination callsign */
	
	/* where does the digipeater path start? */
	via_start = dstcall_end;
	
	/* check if the srccall equals the client's login */
	if (strlen(c->username) == src_end - s && memcmp(c->username, s, (int)(src_end - s)) == 0)
		originated_by_client = 1;
	
	/* if disallow_unverified is enabled, don't allow unverified clients
	 * to send any packets
	 */
	if (disallow_unverified) {
		if (!c->validated)
			return INERR_DISALLOW_UNVERIFIED;
		if (memstr(",TCPXX", via_start, path_end))
			return INERR_DISALLOW_UNVERIFIED_PATH;
	}
	
	/* check if the path contains NOGATE or other signs which tell the
	 * packet should be dropped
	 */
	if (digi_path_drop(via_start, path_end))
		return INERR_NOGATE;
	
	/* check if there are invalid callsigns in the digipeater path before Q */
	if (check_path_calls(via_start, path_end) == -1)
		return INERR_INV_PATH_CALL;
	
	/* check for 3rd party packets */
	if (*(data + 1) == '}') {
		/* if the 3rd-party packet's header has TCPIP or TCPXX, drop it */
		char *party_hdr_end = memchr(data+2, ':', packet_end-data-2);
		if (party_hdr_end) {
			/* TCPIP is more likely, test for it first */
			if ((memstr(",TCPIP", data+2, party_hdr_end)) || (memstr(",TCPXX", data+2, party_hdr_end)) )
				return INERR_3RD_PARTY_IP;
		}
	}
	
	/* process Q construct, path_append_len of path_append will be copied
	 * to the end of the path later
	 */
	path_append_len = q_process( c, s, path_append, sizeof(path_append),
					via_start, &path_end, pathlen, &q_start,
					&q_replace, originated_by_client );
	
	if (path_append_len < 0) {
		/* the q construct algorithm decided to drop the packet */
		hlog(LOG_DEBUG, "%s/%s: q construct drop: %d", c->addr_rem, c->username, path_append_len);
		return path_append_len;
	}
	
	/* get a packet buffer */
	int new_len;
	if (q_replace)
		new_len = len+path_append_len-(path_end-q_replace)+3; /* we remove the old path first */
	else
		new_len = len+path_append_len+3; /* we add path_append_len + CRLFNUL */
	
	/* check if the resulting packet is too long */	
	if (new_len > PACKETLEN_MAX_LARGE) {
		hlog(LOG_DEBUG, "packet too long after inserting new Q construct (%d bytes, max %d)", new_len, PACKETLEN_MAX_LARGE);
		return INERR_LONG_PACKET;
	}
	
	pb = pbuf_get(self, new_len);
	if (!pb) {
		// This should never happen...
		hlog(LOG_ERR, "pbuf_get failed to get packet buffer");
		return INERR_OUT_OF_PBUFS; // No room :-(
	}
	pb->next = NULL; // OPTIMIZE: pbuf arrives pre-zeroed, this could be removed maybe?
	pb->flags = 0;
	
	/* store the source reference */
	pb->origin = c;
	
	/* when it was received ? */
	pb->t = tick;
	
	/* classify the packet as coming from an uplink or client */
	if (c->state == CSTATE_COREPEER || (c->flags & CLFLAGS_UPLINKPORT)) {
		pb->flags |= F_FROM_UPSTR;
	} else if (c->flags & CLFLAGS_DUPEFEED) {
		/* we ignore packets from duplicate feeds */
		rc = 0;
		goto free_pb_ret;
	} else if (c->flags & CLFLAGS_INPORT) {
		pb->flags |= F_FROM_DOWNSTR;
	} else {
		hlog(LOG_ERR, "%s/%s (fd %d): incoming_parse failed to classify packet", c->addr_rem, c->username, c->fd);
		rc = INERR_CLASS_FAIL;
		goto free_pb_ret;
	}
	
	/* if the packet is sourced by a local login, but the packet is not
	 * originated by that station, drop it.
	 */
	if (!originated_by_client && clientlist_check_if_validated_client(s, src_end - s) != -1) {
		/* We mark the packet as a dupe, since dropping it completely
		 * would result in an error-type counter getting incremented.
		 * This is slightly incorrect (perhaps the packet is not a
		 * duplicate after all), probably there should be a separate
		 * statistics counter for this. TODO: add a counter.
		 */
		//hlog(LOG_DEBUG, "%s/%s: dropping due to source call '%.*s' being logged in on another socket", c->addr_rem, c->username, src_end - s, s);
		pb->flags |= F_DUPE;
	}
	
	/* Copy the unmodified part of the packet header */
	if (q_replace) {
		/* if we're replacing the Q construct, we don't copy the old one */
		memcpy(pb->data, s, q_replace - s);
		p = pb->data + (q_replace - s);
	} else {
		memcpy(pb->data, s, path_end - s);
		p = pb->data + (path_end - s);
	}
	
	/* If q_process left q_start unmodified (as NULL), it wants to say
	 * that it produced a new Q construct, which is returned in
	 * path_append. If it points somewhere in the header, then fine,
	 * it points to an existing Q construct.
	 */
	if (q_start == NULL && path_append_len > 0) {
		pb->qconst_start = p + 1;
	} else if (q_start > s && q_start < path_end) {
		pb->qconst_start = pb->data + (q_start - s);
	} else {
		/* If this ever happened, it'd be bad: we didn't find an
		 * existing Q construct and we didn't pick one to insert.
		 */
		hlog(LOG_INFO, "%s/%s: q construct bug: did not find a good construct or produce a new one for:\n%s\n", c->addr_rem, c->username, s);
		rc = INERR_Q_BUG;
		goto free_pb_ret;
	}
	
	/* Copy the modified or appended part of the packet header -- qcons */
	memcpy(p, path_append, path_append_len);
	p += path_append_len;
	
	//hlog(LOG_DEBUG, "q construct: %.*s", 3, pb->qconst_start);
	
	/* Copy the unmodified end of the packet (including the :) */
	memcpy(p, info_start - 1, datalen);
	info_start = p + 1;
	p += datalen;
	memcpy(p, "\r\n", 3); /* append missing CRLFNUL,
				 the NUL is implied in C-style ASCIIZ strings  */
	p += 2; /* We ignore the convenience NUL. */
	
	/* How much there really is data? */
	pb->packet_len = p - pb->data;
	
	packet_end = p; /* for easier overflow checking expressions */
	/* fill necessary info for parsing and dupe checking in the packet buffer */
	pb->srcname = pb->data;
	pb->srcname_len = src_end - s;
	pb->srccall_end = pb->data + (src_end - s);
	pb->dstcall_end_or_ssid = pb->data + (dstcall_end_or_ssid - s);
	pb->dstcall_end = pb->data + (dstcall_end - s);
	pb->dstcall_len = via_start - src_end - 1;
	pb->info_start  = info_start;
	
	//hlog(LOG_DEBUG, "After parsing and Qc algorithm: %.*s", pb->packet_len-2, pb->data);
	
	/* just try APRS parsing */
	rc = parse_aprs(pb);
	
	if (rc < 0)
		goto free_pb_ret;
	
	if (rc == 0 && (pb->packettype & T_MESSAGE) && pb->dstname_len == 6
		&& strncasecmp(pb->dstname, "SERVER", 6) == 0) {
		/* This is a message from a client destined to the local server.
		 * Process it!
		 */
		if (!originated_by_client) {
			hlog(LOG_DEBUG, "message to SERVER from non-local client %.*s, dropping", pb->srcname_len, pb->srcname);
			goto free_pb_ret;
		}
		
		rc = incoming_server_message(self, c, pb);
		goto free_pb_ret;
	}
	
	/* check for general queries - those cause reply floods and need
	 * to be dropped
	 */
	if (pb->packettype & T_QUERY) {
		rc = INERR_GENERAL_QUERY;
		goto free_pb_ret;
	}
	
	/* Filter preprocessing before sending this to dupefilter.. */
	filter_preprocess_dupefilter(pb);
	
	/* If the packet came in on a filtered port, mark the station as
	 * heard on this port, so that messages can be routed to it.
	 */
	if (c->flags & CLFLAGS_IGATE)
		client_heard_update(c, pb);
	
	/* put the buffer in the thread's incoming queue */
	*self->pbuf_incoming_local_last = pb;
	self->pbuf_incoming_local_last = &pb->next;
	self->pbuf_incoming_local_count++;
	
	return rc;
	
	/* in case the packet does not go to the incoming queue, free it up */
free_pb_ret:
	pbuf_free(self, pb);
	return rc;
}

/*
 *	Handler called once for each input APRS-IS line by the socket reading function
 *	for normal APRS-IS traffic.
 */

int incoming_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len)
{
	int e;
	
	/* note: len does not include CRLF, it's reconstructed here... we accept
	 * CR, LF or CRLF on input but make sure to use CRLF on output.
	 */
	
	/* Make sure it looks somewhat like an APRS-IS packet... len is without CRLF.
	 * Do not do PACKETLEN_MIN test here, since it would drop the 'filter'
	 * commands.
	 */
	if (len > PACKETLEN_MAX-2) {
		e = INERR_LONG_PACKET;
		goto in_drop;
	}

	/* starts with '#' => a comment packet, timestamp or something */
	if (*s == '#') {
		hlog(LOG_DEBUG, "%s/%s: #-in: '%.*s'", c->addr_rem, c->username, len, s);
		if (l4proto != IPPROTO_UDP && (c->flags & CLFLAGS_USERFILTEROK)) {
			/* filter adjunct commands ? */
			char *filtercmd = memcasestr("filter", s, s + len);
			if (filtercmd)
				return filter_commands(self, c, 0, filtercmd, len - (filtercmd - s));
			
		}
		
		return 0;
	}
	
	/* parse and process the packet */
	e = incoming_parse(self, c, s, len);

in_drop:	
	if (e < 0) {
		/* failed parsing */
		hlog(LOG_DEBUG, "%s/%s: Dropped packet (%d: %s) len %d: %.*s",
			c->addr_rem, c->username, e, incoming_strerror(e), len, len, s);
	}
	
	/* Account the one incoming packet.
	 * Incoming bytes were already accounted earlier.
	 */
	clientaccount_add(c, l4proto, 0, 1, 0, 0, (e < 0) ? e : 0, 0);
	
	return 0;
}

#ifndef _FOR_VALGRIND_
void incoming_cell_stats(struct cellstatus_t *cellst_pbuf_small,
	struct cellstatus_t *cellst_pbuf_medium,
	struct cellstatus_t *cellst_pbuf_large)
{
	cellstatus(pbuf_cells_small, cellst_pbuf_small);
	cellstatus(pbuf_cells_medium, cellst_pbuf_medium);
	cellstatus(pbuf_cells_large, cellst_pbuf_large);
}
#endif
