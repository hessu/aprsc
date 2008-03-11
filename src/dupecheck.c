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
#include "worker.h"
#include "crc32.h"
#include "historydb.h"

int dupecheck_shutting_down;
int dupecheck_running;
pthread_t dupecheck_th;

int pbuf_global_count;
int pbuf_global_dupe_count;

struct dupe_record_t {
	struct dupe_record_t *next;
	uint32_t crc;
	time_t	 t;
	int	 alen;	// Address length
	int	 plen;	// Payload length
	char	 addresses[20];
	char	*packet;
#ifndef _FOR_VALGRIND_
	char	 packetbuf[200];
#else
	char	 packetbuf[1];
#endif
};

struct dupe_record_t **dupecheck_db; /* hash index table */
int dupecheck_db_size = 8192; /* Hash index table size */

#ifndef _FOR_VALGRIND_
struct dupe_record_t *dupecheck_free;
cellarena_t *dupecheck_cells;
#endif


int dupecheck_incount;

/*
 *	Global pbuf purger cleans out pbufs that are too old..
 */
void global_pbuf_purger(void)
{
	struct pbuf_t *pb, *pb2;
	struct pbuf_t *freeset[1000];
	int n, n1, n2;
	int pbuf_global_count_limit      = 30000; // real criteria should be expirer..
	int pbuf_global_dupe_count_limit =  3000; // .. but in simulation our timers are not useful.

	pb = pbuf_global;
	n = 0;
	n1 = 0;
	while (pbuf_global_count > pbuf_global_count_limit && pb && pb->next && pb->next->next) {
	  freeset[n++] = pb;
	  ++n1;
	  pb2 = pb->next; pb->next = NULL; pb = pb2;
	  if (n >= 1000) {
	    pbuf_free_many(freeset, n);
	    n = 0;
	  }
	  --pbuf_global_count;
	}
	pbuf_global = pb;
	if (n > 0) {
	  pbuf_free_many(freeset, n);
	  n = 0;
	}

	pb = pbuf_global_dupe;
	n = 0;
	n2 = 0;
	while (pbuf_global_dupe_count > pbuf_global_dupe_count_limit && pb && pb->next && pb->next->next) {
	  freeset[n++] = pb;
	  ++n2;
	  pb2 = pb->next; pb->next = NULL; pb = pb2;
	  if (n >= 1000) {
	    pbuf_free_many(freeset, n);
	    n = 0;
	  }
	  --pbuf_global_dupe_count;
	}
	pbuf_global = pb;
	if (n > 0) {
	  pbuf_free_many(freeset, n);
	  n = 0;
	}
	pbuf_global_dupe = pb;

	hlog(LOG_DEBUG, "global_pbuf_purger()  freed %d/%d main pbufs, %d/%d dupe bufs", n1, pbuf_global_count, n2, pbuf_global_dupe_count);
}



/*
 *	The cellmalloc does not need internal MUTEX, it is being used in single thread..
 */

void dupecheck_init(void)
{
#ifndef _FOR_VALGRIND_
	dupecheck_db = hmalloc(sizeof(void*) * dupecheck_db_size);
	dupecheck_cells = cellinit( sizeof(struct dupe_record_t), __alignof__(struct dupe_record_t),
				    CELLMALLOC_POLICY_LIFO | CELLMALLOC_POLICY_NOMUTEX,
				    512 /* 512 kB at the time */,  0 /* minfree */);
#endif
}

struct dupe_record_t *dupecheck_db_alloc(int alen, int pktlen)
{
	struct dupe_record_t *dp;
#ifndef _FOR_VALGRIND_
	if (dupecheck_free) {
	  dp = dupecheck_free;
	  dupecheck_free = dp->next;
	} else
	  dp = cellmalloc(dupecheck_cells);
	if (!dp)
	  return NULL;
	dp->alen = alen;
	dp->plen = pktlen;
	dp->next = NULL;
	if (pktlen > sizeof(dp->packetbuf))
	  dp->packet = hmalloc(pktlen+1);
	else
	  dp->packet = dp->packetbuf;

#else
	dp = hmalloc(pktlen + sizeof(*dp));
	memset(dp, 0, sizeof(*dp));
	dp->alen = alen;
	dp->plen = pktlen;
	dp->packet = dp->packetbuf;
#endif

	return dp;
}

void dupecheck_db_free(struct dupe_record_t *dp)
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

int dupecheck(struct pbuf_t *pb)
{
	/* check a single packet */
	// pb->flags |= F_DUPE; /* this is a duplicate! */

	int i;
	int addrlen;  // length of the address part
	int datalen;  // length of the payload
	uint32_t crc;
	const char *addr;
	const char *data;
	struct dupe_record_t **dpp, *dp;
	time_t expiretime = now -  dupefilter_storetime;

	pb->seqnum = ++dupecheck_incount;

	// 1) collect canonic rep of the packet
	addr    = pb->data;
	addrlen = pb->dstcall_end - addr;

	data    = pb->info_start;
	datalen = pb->packet_len - (data - pb->data);

	// Canonic tail has no SPACEs in data portion!
	// TODO: how to treat 0 bytes ???
	while (datalen > 0 && data[datalen-1] == ' ')
	  --datalen;

	// there are no 3rd-party frames in APRS-IS ...

	// 2) calculate checksum (from disjoint memory areas)

	crc = crc32n(addr, addrlen, 0);
	crc = crc32n(data, datalen, crc);

	// 3) lookup if same checksum is in some hash bucket chain
	//  3b) compare packet...
	//    3b1) flag as F_DUPE if so
	i = crc % dupecheck_db_size;
	dpp = &dupecheck_db[i];
	while (*dpp) {
	  dp = *dpp;
	  if (dp->t < expiretime) {
	    // Too old, discard
	    *dpp = dp->next;
	    dupecheck_db_free(dp);
	    continue;
	  }
	  if (dp->crc == crc) {
	    // CRC match!
	    if (dp->alen == addrlen &&
		dp->plen == datalen &&
		memcmp(addr, dp->addresses, addrlen) == 0 &&
		memcmp(data, dp->packet,    datalen) == 0) {
	      // PACKET MATCH!
	      pb->flags |= F_DUPE;
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

	dp = dupecheck_db_alloc(addrlen, datalen);
	if (!dp) return -1; // alloc error!

	*dpp = dp;
	dp->crc = crc;
	dp->t   = now;
	memcpy(dp->addresses, addr, addrlen);
	memcpy(dp->packet,    data, datalen);

	return 0;
}

/*
 *	Dupecheck thread
 */

void dupecheck_thread(void)
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
	
	pthreads_profiling_reset();

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

	hlog(LOG_INFO, "Dupecheck thread started.");

	while (!dupecheck_shutting_down) {
		n = d = 0;
		pb_out = NULL;
		pb_out_prevp = &pb_out;
		pb_out_dupe = NULL;
		pb_out_dupe_prevp = &pb_out_dupe;
		pb_out_count = pb_out_dupe_count = 0;
		pb_out_last = pb_out_dupe_last = NULL;

		/* walk through worker threads */
		for (w = worker_threads; (w); w = w->next) {
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

			hlog(LOG_DEBUG, "Dupecheck got %d packets from worker %d; n=%d",
			     c, w->id, dupecheck_incount);

			for (pb = pb_list; (pb); pb = pbnext) {
				int rc = dupecheck(pb);
				pbnext = pb->next; // it may get modified below..

				if (rc == 0) { // Not duplicate
					*pb_out_prevp = pb;
					 pb_out_prevp = &pb->next;
					 pb_out_last  = pb;
					++pb_out_count;
				} else {       // Duplicate
					*pb_out_dupe_prevp = pb;
					 pb_out_dupe_prevp = &pb->next;
					 pb_out_dupe_last  = pb;
					++pb_out_dupe_count;
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
global_pbuf_purger();
		if (n > 0)
		  hlog(LOG_DEBUG, "Dupecheck did analyze %d packets, found %d duplicates", n, pb_out_dupe_count);
		/* sleep a little, if there was nothing to do */
		if (n == 0)
			poll(NULL, 0, 100); // 100 ms
	}
	
	hlog(LOG_INFO, "Dupecheck thread shut down; incount=%ld", dupecheck_incount);
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
	
	if (pthread_create(&dupecheck_th, NULL, (void *)dupecheck_thread, NULL))
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

