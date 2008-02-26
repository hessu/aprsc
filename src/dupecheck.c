
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
#include "worker.h"

int dupecheck_shutting_down = 0;
int dupecheck_running = 0;
pthread_t dupecheck_th;

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

void dupecheck(struct pbuf_t *pb)
{
	/* FIXME: check a single packet */
	// pb->flags |= F_DUPE; /* this is a duplicate! */
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
			
			*pb_out_prevp = pb_list;
			for (pb = pb_list; (pb); pb = pb->next) {
				dupecheck(pb);
				pb_out_last = pb;
				pb_out_prevp = &pb->next;
			}
			
			n++;
		}
		
		/* put packets in the global buffer */
		if (pb_out) {
			if ((e = rwl_rdlock(&pbuf_global_rwlock))) {
				hlog(LOG_CRIT, "dupecheck: Failed to rdlock pbuf_global_rwlock!");
				exit(1);
			}
			*pbuf_global_prevp = pb_out;
			pbuf_global_prevp = pb_out_prevp;
			pbuf_global_last = pb_out_last;
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

