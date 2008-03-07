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

int dupecheck_shutting_down = 0;
int dupecheck_running = 0;
pthread_t dupecheck_th;

struct dupe_record_t {
	struct dupe_record_t *next;
	uint32_t crc;
	time_t	 t;
	int	 alen;	// Address length
	int	 plen;	// Payload length
	char	 addresses[20];
	char	*packet;
	char	 packetbuf[200];
};

struct dupe_record_t **dupecheck_db;
int dupecheck_db_size = 8192;


cellarena_t *dupecheck_cells;

/*
 *	The cellmalloc does not need internal MUTEX, it is being used in single thread..
 */

void dupecheck_init(void)
{
	dupecheck_db = hmalloc(sizeof(void*) * dupecheck_db_size);
	dupecheck_cells = cellinit( sizeof(struct dupe_record_t), __alignof__(struct dupe_record_t),
				    CELLMALLOC_POLICY_LIFO | CELLMALLOC_POLICY_NOMUTEX,
				    512 /* 512 kB at the time */,  0 /* minfree */);
}

struct dupe_record_t *dupecheck_db_alloc(int alen, int pktlen)
{
	struct dupe_record_t *dp = cellmalloc(dupecheck_cells);
	if (!dp)
	  return NULL;
	dp->alen = alen;
	dp->plen = pktlen;
	dp->next = NULL;
	if (pktlen > sizeof(dp->packetbuf))
	  dp->packet = hmalloc(pktlen+1);
	else
	  dp->packet = dp->packetbuf;

	return dp;
}

void dupecheck_db_free(struct dupe_record_t *dp)
{
	if (dp->packet != dp->packetbuf)
		hfree(dp->packet);
	cellfree(dupecheck_cells, dp);
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
	/* FIXME: check a single packet */
	// pb->flags |= F_DUPE; /* this is a duplicate! */

	int i;
	int addrlen;  // length of the address part
	int datalen;  // length of the payload
	uint32_t crc;
	const char *addr;
	const char *data;
	struct dupe_record_t **dpp, *dp;
	time_t expiretime = now -  dupefilter_storetime;

	// 1) collect canonic rep of the packet
	data    = pb->info_start;
	datalen = pb->packet_len - (data - pb->data);
	addr    = pb->data;
	addrlen = pb->dstcall_end - addr;

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
	    dpp = &dp->next;
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
	  }
	  dpp = &dp->next;
	}
	// dpp points to pointer at the tail of the chain

	// 4) Add comparison copy of non-dupe into dupe-db

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
	struct pbuf_t *pb_list, *pb;
	struct pbuf_t *pb_out, *pb_out_last, **pb_out_prevp;
	struct pbuf_t *pb_out_dupe, *pb_out_dupe_last, **pb_out_dupe_prevp;
	int n;
	int e;
	
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
		n = 0;
		pb_out = NULL;
		pb_out_last = NULL;
		pb_out_prevp = &pb_out;
		pb_out_dupe = NULL;
		pb_out_dupe_last = NULL;
		pb_out_dupe_prevp = &pb_out_dupe;

		/* walk through worker threads */
		for (w = worker_threads; (w); w = w->next) {
			/* if there are items in the worker's pbuf_incoming, grab them and process */
			if (!w->pbuf_incoming)
				continue;

			pthread_mutex_lock(&w->pbuf_incoming_mutex);
			pb_list = w->pbuf_incoming;
			w->pbuf_incoming = NULL;
			w->pbuf_incoming_last = &w->pbuf_incoming;
			pthread_mutex_unlock(&w->pbuf_incoming_mutex);

			//hlog(LOG_DEBUG, "dupecheck got packets from worker %d", w->id);

			for (pb = pb_list; (pb); pb = pb->next) {
				int rc = dupecheck(pb);
				if (rc == 0) { // Not duplicate
					pb_out_last = pb;
					pb_out_prevp = &pb->next;
				} else {       // Duplicate
					pb_out_dupe_last = pb;
					pb_out_dupe_prevp = &pb->next;
				}
			}

			n++;
		}

		/* put packets in the global buffer */
		if (pb_out || pb_out_dupe) {
			if ((e = rwl_rdlock(&pbuf_global_rwlock))) {
				hlog(LOG_CRIT, "dupecheck: Failed to rdlock pbuf_global_rwlock!");
				exit(1);
			}

			if (pb_out) {
				*pbuf_global_prevp = pb_out;
				pbuf_global_prevp = pb_out_prevp;
				pbuf_global_last = pb_out_last;
			}

			if (pb_out_dupe) {
				*pbuf_global_dupe_prevp = pb_out_dupe;
				pbuf_global_dupe_prevp = pb_out_dupe_prevp;
				pbuf_global_dupe_last = pb_out_dupe_last;
			}

			if ((e = rwl_rdunlock(&pbuf_global_rwlock))) {
				hlog(LOG_CRIT, "dupecheck: Failed to rdunlock pbuf_global_rwlock!");
				exit(1);
			}
		}
		/* sleep a little, if there was nothing to do */
		if (n == 0)
			usleep(100 * 1000);
	}
	
	hlog(LOG_DEBUG, "Dupecheck thread shut down.");
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

