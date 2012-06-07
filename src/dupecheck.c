/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

/*
 *	dupecheck.c: the dupe-checking thread
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>

#include "dupecheck.h"
#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "cellmalloc.h"
#include "keyhash.h"
#include "filter.h"
#include "historydb.h"

extern int uplink_simulator;


int dupecheck_shutting_down;
int dupecheck_running;
pthread_t dupecheck_th;
long dupecheck_cellgauge;

int pbuf_global_count;
int pbuf_global_dupe_count;

int pbuf_global_count_limit      =  5000; /* Real criteria is expirer..		 */
int pbuf_global_dupe_count_limit =     1; /* .. but we set some minimum packet counts
					     into the global pbuf queue anyway.  */

long long dupecheck_outcount;  /* 64 bit counters for statistics */
long long dupecheck_dupecount;

#define DUPECHECK_DB_SIZE 8192        /* Hash index table size */
struct dupe_record_t *dupecheck_db[DUPECHECK_DB_SIZE]; /* Hash index table      */

#ifndef _FOR_VALGRIND_
struct dupe_record_t *dupecheck_free;
cellarena_t *dupecheck_cells;
#endif


volatile uint32_t  dupecheck_seqnum      = -2000; // Explicit early wrap-around..
volatile uint32_t  dupecheck_dupe_seqnum = -2000; // Explicit early wrap-around..

static int pbuf_seqnum_lag(const uint32_t seqnum, const uint32_t pbuf_seq)
{
	// The lag calculation method takes care of the value space
	// wrap-around, thus this is not limited on on first 4 billion
	// packets, or whatever smallish..  As long as there are less
	// than 2 billion packets in the in-core value spaces.

	int lag = (int32_t)(seqnum - pbuf_seq);

	if (pbuf_seq == 0)	// Worker without data.
		lag = 2000000000;
	// Presumption is that above mentioned situation has very short
	// existence, but as it can happen, we flag it with such a high
	// value that temporarily the global_pbuf_purger() will not
	// purge any items out of the global queue.
	return lag;
}


/*
 *	Global pbuf purger cleans out pbufs that are too old..
 */
static void global_pbuf_purger(const int all, int pbuf_lag, int pbuf_dupe_lag)
{
	struct pbuf_t *pb, *pb2;
	struct pbuf_t *freeset[2002];
	int n, n1, n2, lag;
	time_t lastage1 = 0, lastage2 = 0;

	time_t expire2 = now - pbuf_global_dupe_expiration;
	time_t expire1 = now - pbuf_global_expiration;

	static int show_zeros = 1;

	if (all) {
		pbuf_global_count_limit       = 0;
		pbuf_global_dupe_count_limit  = 0;
		expire1  = expire2       = now+10;
	}

	pb = pbuf_global;
	if (pb) lastage1 = pb->t;
	n = 0;
	n1 = 0;
	while ( pbuf_global_count > pbuf_global_count_limit && pb ) {

		lastage1 = pb->t;
		if (pb->t >= expire1)
			break; // stop at newer than expire1
		lag = pbuf_seqnum_lag(dupecheck_seqnum, pb->seqnum);
		if (pbuf_lag >= lag)
			break; // some output-worker is lagging behind this item!

		freeset[n++] = pb;
		++n1;
		--pbuf_global_count;
		// dissociate the pbuf from the chain
		pb2 = pb->next; pb->next = NULL; pb = pb2;
		if (n >= 2000) {
			pbuf_free_many(freeset, n);
			n = 0;
		}
	}
	pbuf_global = pb;
	if (n > 0) {
		pbuf_free_many(freeset, n);
	}

	pb = pbuf_global_dupe;
	if (pb) lastage2 = pb->t;
	n = 0;
	n2 = 0;
	while ( pbuf_global_dupe_count > pbuf_global_dupe_count_limit && pb ) {

		lastage2 = pb->t;
		if (pb->t >= expire2)
			break; // stop at newer than expire2
		lag = pbuf_seqnum_lag(dupecheck_dupe_seqnum, pb->seqnum);
		if (pbuf_dupe_lag >= lag)
			break; // some output-worker is lagging behind this item!

		freeset[n++] = pb;
		++n2;
		--pbuf_global_dupe_count;
		// dissociate the pbuf from the chain
		pb2 = pb->next; pb->next = NULL; pb = pb2;
		if (n >= 2000) {
			pbuf_free_many(freeset, n);
			n = 0;
		}
	}
	pbuf_global_dupe = pb;
	if (n > 0) {
		pbuf_free_many(freeset, n);
	}

	// debug printout time...  map "undefined" lag values to zero.

	if (pbuf_lag      == 2000000000) pbuf_lag      = 0;
	if (pbuf_dupe_lag == 2000000000) pbuf_dupe_lag = 0;

	if (lastage1 == 0) lastage1 = now+2; // makes printout of "-2" (or "-1")
	if (lastage2 == 0) lastage2 = now+2;

	if (show_zeros || n1 || n2 || pbuf_lag  || pbuf_dupe_lag) {
		// report only when there is something to report...
		hlog( LOG_DEBUG,
		      "global_pbuf_purger()  freed %d/%d main pbufs, %d/%d dupe bufs, lags: %d/%d  Ages: %d/%d",
		      n1, pbuf_global_count, n2, pbuf_global_dupe_count, pbuf_lag, pbuf_dupe_lag,
		      (int)(now-lastage1), (int)(now-lastage2) );

		if (!(n1 || n2 || pbuf_lag || pbuf_dupe_lag))
			show_zeros = 0;
		else
			show_zeros = 1;
	}
}



/*
 *	The cellmalloc does not need internal MUTEX, it is being used in single thread..
 */

void dupecheck_init(void)
{
#ifndef _FOR_VALGRIND_
	dupecheck_cells = cellinit( "dupecheck",
				    sizeof(struct dupe_record_t),
				    __alignof__(struct dupe_record_t),
				    CELLMALLOC_POLICY_LIFO | CELLMALLOC_POLICY_NOMUTEX,
				    2048 /* 2 MB at the time */,
				    0 /* minfree */);
#endif
}

static struct dupe_record_t *dupecheck_db_alloc(int alen, int pktlen)
{
	struct dupe_record_t *dp;
#ifndef _FOR_VALGRIND_
	if (dupecheck_free) { /* pick from free chain */
		dp = dupecheck_free;
		dupecheck_free = dp->next;
	} else
		dp = cellmalloc(dupecheck_cells);
	if (!dp)
		return NULL;
#else
	dp = hmalloc(pktlen + sizeof(*dp));
#endif
	memset(dp, 0, sizeof(*dp));
	dp->alen = alen;
	dp->plen = pktlen;
	dp->packet = dp->packetbuf;
	if (pktlen > sizeof(dp->packetbuf))
		dp->packet = hmalloc(pktlen+1);

	++dupecheck_cellgauge;

	return dp;
}

static void dupecheck_db_free(struct dupe_record_t *dp)
{
#ifndef _FOR_VALGRIND_
	if (dp->packet != dp->packetbuf)
		hfree(dp->packet);
	dp->next = dupecheck_free;
	dupecheck_free = dp;
	// cellfree(dupecheck_cells, dp);
#else
	hfree(dp);
#endif
	--dupecheck_cellgauge;
}

/*	The  dupecheck_cleanup() is for regular database cleanups,
 *	Call this about once a minute.
 *
 *	Note: entry validity is possibly shorter time than the cleanup
 *	invocation interval!
 */
static void dupecheck_cleanup(void)
{
	struct dupe_record_t *dp, **dpp;
	time_t expiretime = now - dupefilter_storetime;
	int cleancount = 0, i;
	

	for (i = 0; i < DUPECHECK_DB_SIZE; ++i) {
		dpp = & dupecheck_db[i];
		while (( dp = *dpp )) {
			if (dp->t < expiretime) {
				/* Old..  discard. */
				*dpp = dp->next;
				dp->next = NULL;
				dupecheck_db_free(dp);
				++cleancount;
				continue;
			}
			/* No expiry, just advance the pointer */
			dpp = &dp->next;
		}
	}
	// hlog( LOG_DEBUG, "dupecheck_cleanup() removed %d entries, count now %ld",
	//       cleancount, dupecheck_cellgauge );
}

/*
 *	signal handler
 */
 
int dupecheck_sighandler(int signum)
{
	switch (signum) {
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)dupecheck_sighandler);	/* restore handler */
	return 0;
}

/*
 *	check a single packet for duplicates
 */

static int dupecheck(struct pbuf_t *pb)
{
	/* check a single packet */
	// pb->flags |= F_DUPE; /* this is a duplicate! */

	int i;
	int addrlen;  // length of the address part
	int datalen;  // length of the payload
	uint32_t hash, idx;
	const char *addr;
	const char *data;
	struct dupe_record_t **dpp, *dp;
	time_t expiretime = now -  dupefilter_storetime;

	// 1) collect canonic rep of the packet
	addr    = pb->data;
	addrlen = pb->dstcall_end_or_ssid - addr;

	data    = pb->info_start;
	datalen = pb->packet_len - (data - pb->data);

	// Canonic tail has no SPACEs in data portion!
	// TODO: how to treat 0 bytes ???
	while (datalen > 0 && data[datalen-1] == ' ')
	  --datalen;


	// there are no 3rd-party frames in APRS-IS ...

	// 2) calculate checksum (from disjoint memory areas)

	hash = keyhash(addr, addrlen, 0);
	hash = keyhash(data, datalen, hash);
	idx  = hash;

	// 3) lookup if same checksum is in some hash bucket chain
	//  3b) compare packet...
	//    3b1) flag as F_DUPE if so
	idx ^= (idx >> 13); /* fold the hash bits.. */
	idx ^= (idx >> 26); /* fold the hash bits.. */
	i = idx % DUPECHECK_DB_SIZE;
	dpp = &dupecheck_db[i];
	while (*dpp) {
		dp = *dpp;
		if (dp->hash == hash &&
		    dp->t > expiretime) {
			// HASH match!  And not too old!
			if (dp->alen == addrlen &&
			    dp->plen == datalen &&
			    memcmp(addr, dp->addresses, addrlen) == 0 &&
			    memcmp(data, dp->packet,    datalen) == 0) {
				// PACKET MATCH!
				//hlog(LOG_DEBUG, "Dupe: %.*s", pb->packet_len - 2, pb->data);
				//hlog(LOG_DEBUG, "Orig: %.*s %.*s", addrlen, dp->addresses, datalen, dp->packet);
				pb->flags |= F_DUPE;
				filter_postprocess_dupefilter(pb);
				return F_DUPE;
			}
			// no packet match.. check next
		}
		dpp = &dp->next;
	}
	// dpp points to pointer at the tail of the chain

	// 4) Add comparison copy of non-dupe into dupe-db
	//    .. and historydb wants also copy..

	historydb_insert(pb);
	filter_postprocess_dupefilter(pb);

	dp = dupecheck_db_alloc(addrlen, datalen);
	if (!dp) return -1; // alloc error!

	*dpp = dp;
	memcpy(dp->addresses, addr, addrlen);
	memcpy(dp->packet,    data, datalen);
	dp->hash = hash;
	dp->t   = now; /* Use the current timestamp instead of the arrival time.
	                  If our incoming worker, or dupecheck, is lagging for
	                  reason or another (for example, a huge incoming burst
	                  of traffic), using the arrival time instead of current
	                  time could make the dupecheck db entry expire too early.
	                  In an extreme trouble case, we could expire dupecheck db
	                  entries very soon after the packet has gone out from us,
	                  which would make loops more likely and possibly increase
	                  the traffic and make us lag even more.
	                  This timestamp should be closer to the *outgoing* time
	                  than the *incoming* time, and current timestamp is a
	                  good middle ground. Simulator is not important.
			*/
	return 0;
}

/*
 *	Worker asks for info on outgoing lag to adjust its main-loop delays
 *	and priorities
 */

int  outgoing_lag_report(struct worker_t *self, int *lag, int *dupelag)
{
	int lag1 = pbuf_seqnum_lag(dupecheck_seqnum, self->last_pbuf_seqnum);
	int lag2 = pbuf_seqnum_lag(dupecheck_dupe_seqnum, self->last_pbuf_dupe_seqnum);

	if (lag)     *lag     = lag1;
	if (dupelag) *dupelag = lag2;

	if (lag1 == 2000000000) lag1 = 0;
	if (lag2 == 2000000000) lag2 = 0;

	if (lag1 < lag2) lag1 = lag2;

	return lag1; // Higher of the two..
}



/*
 *	Dupecheck thread
 */

static void dupecheck_thread(void)
{
	sigset_t sigs_to_block;
	struct worker_t *w;
	struct pbuf_t *pb_list, *pb, *pbnext;
	struct pbuf_t *pb_out, **pb_out_prevp, *pb_out_last;
	struct pbuf_t *pb_out_dupe, **pb_out_dupe_prevp, *pb_out_dupe_last;
	int n;
	int e;
	int c, d;
	int pb_out_count, pb_out_dupe_count;
	int worker_pbuf_lag;
	int worker_pbuf_dupe_lag;
	time_t cleanup_tick = now;
	
	pthreads_profiling_reset("dupecheck");

	sigemptyset(&sigs_to_block);
	sigaddset(&sigs_to_block, SIGALRM);
	sigaddset(&sigs_to_block, SIGINT);
	sigaddset(&sigs_to_block, SIGTERM);
	sigaddset(&sigs_to_block, SIGQUIT);
	sigaddset(&sigs_to_block, SIGHUP);
	sigaddset(&sigs_to_block, SIGURG);
	sigaddset(&sigs_to_block, SIGPIPE);
	sigaddset(&sigs_to_block, SIGUSR1);
	sigaddset(&sigs_to_block, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);

	hlog(LOG_INFO, "Dupecheck thread ready.");

	while (!dupecheck_shutting_down) {
		n = d = 0;
		pb_out       = NULL;
		pb_out_prevp = &pb_out;
		pb_out_dupe  = NULL;
		pb_out_dupe_prevp = &pb_out_dupe;
		pb_out_count      = pb_out_dupe_count    = 0;
		pb_out_last       = pb_out_dupe_last     = NULL;
		worker_pbuf_lag   = worker_pbuf_dupe_lag = -1;

		/* walk through worker threads */
		for (w = worker_threads; (w); w = w->next) {

			/* find the highest worker lag count... */

			/* It really does not matter when we do this calculation, the worker-threads
			   are running independently of us, and only thing guaranteed for us is that
			   they are progressing onwards the pbuf sequences. */
			c = pbuf_seqnum_lag(dupecheck_seqnum, w->last_pbuf_seqnum);
			if (w->last_pbuf_seqnum == 0) c = 2000000000;
			if (c > worker_pbuf_lag)
			  worker_pbuf_lag = c;
			c = pbuf_seqnum_lag(dupecheck_dupe_seqnum, w->last_pbuf_dupe_seqnum);
			if (c > worker_pbuf_dupe_lag)
			  worker_pbuf_dupe_lag = c;

			/* if there are items in the worker's pbuf_incoming, grab them and process */
			if (!w->pbuf_incoming)
				continue;

			pthread_mutex_lock(&w->pbuf_incoming_mutex);
			pb_list = w->pbuf_incoming;
			w->pbuf_incoming = NULL;
			w->pbuf_incoming_last = &w->pbuf_incoming;
			c = w->pbuf_incoming_count;
			w->pbuf_incoming_count = 0;
			pthread_mutex_unlock(&w->pbuf_incoming_mutex);

			//hlog(LOG_DEBUG, "Dupecheck got %d packets from worker %d; n=%d",
			//     c, w->id, dupecheck_seqnum);

			for (pb = pb_list; (pb); pb = pbnext) {
				int rc = dupecheck(pb);
				pbnext = pb->next; // it may get modified below..

				if (rc == 0) { // Not duplicate
					*pb_out_prevp = pb;
					 pb_out_prevp = &pb->next;
					 pb_out_last  = pb;
					 pb->seqnum = ++dupecheck_seqnum;
					++pb_out_count;
				} else {       // Duplicate
					*pb_out_dupe_prevp = pb;
					 pb_out_dupe_prevp = &pb->next;
					 pb_out_dupe_last  = pb;
					 pb->seqnum = ++dupecheck_dupe_seqnum;
					++pb_out_dupe_count;
					//hlog(LOG_DEBUG, "is duplicate");
				}
				n++;
			}
		}
		// terminate those out-chains in every case..
		*pb_out_prevp = NULL;
		*pb_out_dupe_prevp = NULL;

		/* put packets in the global buffer */
		if (pb_out || pb_out_dupe) {
			if ((e = rwl_rdlock(&pbuf_global_rwlock))) {
				hlog(LOG_CRIT, "dupecheck: Failed to rdlock pbuf_global_rwlock!");
				exit(1);
			}

			if (pb_out) {
				*pbuf_global_prevp = pb_out;
				pbuf_global_prevp  = pb_out_prevp;
				pbuf_global_last   = pb_out_last;
				pbuf_global_count += pb_out_count;
			}

			if (pb_out_dupe) {
				*pbuf_global_dupe_prevp = pb_out_dupe;
				pbuf_global_dupe_prevp  = pb_out_dupe_prevp;
				pbuf_global_dupe_last   = pb_out_dupe_last;
				pbuf_global_dupe_count += pb_out_dupe_count;
			}
			if ((e = rwl_rdunlock(&pbuf_global_rwlock))) {
				hlog(LOG_CRIT, "dupecheck: Failed to rdunlock pbuf_global_rwlock!");
				exit(1);
			}
		}

#if 0
		// If no workers are running, the output processing is also
		// impossible, thus the default lag of "-1" is just fine,
		// and the out-queue purge is not lag-restricted.

		if (worker_pbuf_lag < 0)
			worker_pbuf_lag = 2000000000;
		if (worker_pbuf_dupe_lag < 0)
			worker_pbuf_dupe_lag = 2000000000;
#endif

		dupecheck_outcount  += pb_out_count;
		dupecheck_dupecount += pb_out_dupe_count;

		if (cleanup_tick <= now) { // once in a (simulated) minute or so..
			cleanup_tick = now + 60;

			global_pbuf_purger(0, worker_pbuf_lag, worker_pbuf_dupe_lag);
			dupecheck_cleanup();
		}

		// if (n > 0)
		//    hlog(LOG_DEBUG, "Dupecheck did analyze %d packets, found %d duplicates", n, pb_out_dupe_count);
		/* sleep a little, if there was nothing to do */
		if (n == 0)
			poll(NULL, 0, 50); // 50 ms
	}
	
	hlog( LOG_INFO, "Dupecheck thread shut down; seqnum=%u/%u",
	      pbuf_seqnum_lag(dupecheck_seqnum,(uint32_t)-2000),     // initial bias..
	      pbuf_seqnum_lag(dupecheck_dupe_seqnum,(uint32_t)-2000));

	dupecheck_running = 0;
}

/*
 *	Start / stop dupecheck
 */

void dupecheck_start(void)
{
	if (dupecheck_running)
		return;
	
	dupecheck_shutting_down = 0;
	
	if (pthread_create(&dupecheck_th, &pthr_attrs, (void *)dupecheck_thread, NULL))
		perror("pthread_create failed for dupecheck_thread");
		
	dupecheck_running = 1;
}

void dupecheck_stop(void)
{
	int e;
	
	if (!dupecheck_running)
		return;
	
	dupecheck_shutting_down = 1;
	
	if ((e = pthread_join(dupecheck_th, NULL)))
		hlog(LOG_ERR, "Could not pthread_join dupecheck_th: %s", strerror(e));
	else
		hlog(LOG_INFO, "Dupecheck thread has terminated.");
}

/*	The  dupecheck_atend() is primarily for valgrind() to clean up dupecache.
 */
void dupecheck_atend(void)
{
	int i;
	struct dupe_record_t *dp, *dp2;

	for (i = 0; i < DUPECHECK_DB_SIZE; ++i) {
		dp = dupecheck_db[i];
		while (dp) {
			dp2 = dp->next;
			dupecheck_db_free(dp);
			dp = dp2;
		}
		dupecheck_db[i] = NULL;
	}
#if 0 /* Well, not really...  valgrind did hfree() the dupecells,
	 and without valgrind we really are not interested of freeup of
	 the free chain... */
	dp = dupecheck_free;
	for ( ; dp ; dp = dp2 ) {
		dp2 = dp->next;
		cellfree(dupecheck_cells, dp);
	}
#endif
	global_pbuf_purger(1, -1, -1); // purge everything..
}

/*
 *	cellmalloc status
 */
void dupecheck_cell_stats(struct cellstatus_t *cellst)
{
	// TODO: this is not quite thread safe, but may be OK
	cellstatus(dupecheck_cells, cellst);
}

