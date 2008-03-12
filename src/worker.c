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
 *	worker.c: the worker thread
 */

#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>

#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "worker.h"
#include "login.h"
#include "uplink.h"
#include "incoming.h"
#include "outgoing.h"
#include "filter.h"

time_t now;	/* current time, updated by the main thread, MAY be spun around by the simulator */
time_t tick;	/* real monotonous clock, may or may not be wallclock */

extern int ibuf_size;

struct worker_t *worker_threads = NULL;
int workers_running = 0;
int sock_write_expire  = 60;    /* 60 seconds OK ?       */
int keepalive_interval = 20;    /* 20 seconds for individual socket, NOT all in sync! */
int keepalive_poll_freq = 2;	/* keepalive analysis scan interval */
int obuf_writes_treshold = 5;	/* This many writes per keepalive scan interval switch socket
				   output to buffered. */

/* global packet buffer */
rwlock_t pbuf_global_rwlock = RWL_INITIALIZER;
struct pbuf_t  *pbuf_global = NULL;
struct pbuf_t  *pbuf_global_last = NULL;
struct pbuf_t **pbuf_global_prevp = &pbuf_global;
struct pbuf_t  *pbuf_global_dupe = NULL;
struct pbuf_t  *pbuf_global_dupe_last = NULL;
struct pbuf_t **pbuf_global_dupe_prevp = &pbuf_global_dupe;

/* object alloc/free */

struct client_t *client_alloc(void)
{
	// TODO:  use cellmalloc ?

	struct client_t *c = hmalloc(sizeof(*c));
	memset((void *)c, 0, sizeof(*c));
	c->fd = -1;

	// Sometimes the ibuf_size initializes as zero..
	if (!ibuf_size) ibuf_size = 8100;

	c->ibuf_size = ibuf_size;
	c->ibuf      = hmalloc(c->ibuf_size);

	c->obuf_size = obuf_size;
	c->obuf      = hmalloc(c->obuf_size);

	return c;
}

void client_free(struct client_t *c)
{
	if (c->fd >= 0)	 close(c->fd);
	if (c->ibuf)     hfree(c->ibuf);
	if (c->obuf)     hfree(c->obuf);
	if (c->addr_s)   hfree(c->addr_s);
	if (c->username) hfree(c->username);

	filter_free(c->defaultfilters);
	filter_free(c->userfilters);

	memset(c, 0, sizeof(*c));
	hfree(c);
}


/*
 *	signal handler
 */
 
int worker_sighandler(int signum)
{
	switch (signum) {
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)worker_sighandler);	/* restore handler */
	return 0;
}

/*
 *	close and forget a client connection
 */

void close_client(struct worker_t *self, struct client_t *c)
{
	hlog( LOG_DEBUG, "Worker %d disconnecting %s fd %d: %s",
	      self->id, c->state == CSTATE_CONNECTED ? "client":"uplink", c->fd, c->addr_s);

	/* close */
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	
	/* remove from polling list */
	if (self->xp)
		xpoll_remove(self->xp, c->xfd);
	
	/* link the list together over this node */
	if (c->next)
		c->next->prevp = c->prevp;
	*c->prevp = c->next;

	/* if this happens to be the uplink, tell the uplink module that the
	 * connection has gone away
	 */
	if (c->state == CSTATE_UPLINK)
		uplink_close(c);

	/* free it up */
	client_free(c);
	
	/* reduce client counter */
	self->client_count--;
}

/*
 *	write data to a client (well, at least put it in the output buffer)
 *	(this is also used with len=0 to flush current buffer)
 */

int client_write(struct worker_t *self, struct client_t *c, char *p, int len)
{
	/* Count the number of writes towards this client,  the keepalive
	   manager monitors this counter to determine if the socket should be
	   kept in BUFFERED mode, or written immediately every time.
	   Buffer flushing is done every keepalive_poll_freq (2) seconds.
	*/
	c->obuf_writes++;

	if (c->obuf_end + len > c->obuf_size) {
		/* Oops, cannot append the data to the output buffer.
		 * Check if we can make space for it by moving data
		 * towards the beginning of the buffer?
		 */
		if (len > c->obuf_size - (c->obuf_end - c->obuf_start)) {
			/* Oh crap, the data will not fit even if we move stuff.
			   Lets try writing.. */
			int i, e;
		write_retry:;
			i = write(c->fd, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
			e = errno;
			if (i < 0 && (e == EINTR)) {
			  // retrying..
			  if (c->obuf_start > 0)
			    memmove((void *)c->obuf, (void *)c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
			  c->obuf_end  -= c->obuf_start;
			  c->obuf_start = 0;
			  goto write_retry;
			}
			if (i > 0) {
				c->obuf_start += i;
				c->obuf_wtime = tick;
			}
			/* Is it still out of space ? */
			if (len > c->obuf_size - (c->obuf_end - c->obuf_start)) {
				/* Oh crap, the data will still not fit! */

#if WBUF_ADJUSTER
			  // Fails to write, consider enlarging socket buffer..  Maybe..

			  // Do Socket wbuf size adjustment (double it) and try to write again
			  // ... until some limit.
			  int wbuf = c->wbuf_size;
			  wbuf *= 2;
			  if (wbuf < 300000) { // FIXME: parametrize this limit!
			    c->wbuf_size = wbuf;
			    i = setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &wbuf, sizeof(wbuf));
			    hlog(LOG_DEBUG, "Enlarging client socket wbuf, now %d", wbuf);
			    goto write_retry;
			  }
#endif
			  hlog(LOG_DEBUG, "client_write(%s) fails; %s", c->addr_s, strerror(e));
			  return -1;
			}
		}
		/* okay, move stuff to the beginning to make space in the end */
		if (c->obuf_start > 0)
			memmove((void *)c->obuf, (void *)c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
		c->obuf_end  -= c->obuf_start;
		c->obuf_start = 0;
	}
	
	/* copy data to the output buffer */
	if (len > 0)
		memcpy((void *)c->obuf + c->obuf_end, p, len);
	c->obuf_end += len;
	
	/* Is it over the flush size ? */
	if (c->obuf_end > c->obuf_flushsize || ((len == 0) && (c->obuf_end > c->obuf_start))) {
		int i, e;
	write_retry_2:;
		i = write(c->fd, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
		e = errno;
		if (i < 0 && e == EINTR)
		  goto write_retry_2;
		if (i < 0 && len != 0) {
			hlog(LOG_DEBUG, "client_write(%s) fails; %s", c->addr_s, strerror(e));
			return -1;
		}
		if (i > 0) {
			c->obuf_start += i;
			c->obuf_wtime = tick;
		}
	}
	/* All done ? */
	if (c->obuf_start >= c->obuf_end) {
		c->obuf_start = 0;
		c->obuf_end   = 0;
		return len;
	}

	/* tell the poller that we have outgoing data */
	xpoll_outgoing(self->xp, c->xfd, 1);
	
	return len; 
}

/*
 *	printf to a client
 */

int client_printf(struct worker_t *self, struct client_t *c, const char *fmt, ...)
{
	va_list args;
	char s[PACKETLEN_MAX];
	int i;
	
	va_start(args, fmt);
	i = vsnprintf(s, PACKETLEN_MAX, fmt, args);
	va_end(args);
	
	if (i < 0 || i >= PACKETLEN_MAX) {
		hlog(LOG_ERR, "client_printf failed to %s: '%s'", c->addr_s, fmt);
		return -1;
	}
	
	return client_write(self, c, s, i);
}

/*
 *	tell the client once that it has bad filter definition
 */
void client_bad_filter_notify(struct worker_t *self, struct client_t *c, const char *filt)
{
	if (!c->warned) {
		c->warned = 1;
		client_printf(self, c, "# Warning: Bad filter: %s\r\n", filt);
	}
}



/*
 *	handle an event on an fd
 */

int handle_client_readable(struct worker_t *self, struct client_t *c)
{
	int r;
	char *s;
	char *ibuf_end;
	char *row_start;
	
	r = read(c->fd, c->ibuf + c->ibuf_end, c->ibuf_size - c->ibuf_end - 1);
	if (r == 0) {
		hlog(LOG_DEBUG, "read: EOF from client fd %d (%s)", c->fd, c->addr_s);
		close_client(self, c);
		return -1;
	}
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0; /* D'oh..  return again latter */

		hlog(LOG_DEBUG, "read: Error from client fd %d (%s): %s", c->fd, c->addr_s, strerror(errno));
		hlog(LOG_DEBUG, " .. ibuf=%p  ibuf_end=%d  ibuf_size=%d", c->ibuf, c->ibuf_end, c->ibuf_size-c->ibuf_end-1);
		close_client(self, c);
		return -1;
	}
	c->ibuf_end += r;
	//hlog(LOG_DEBUG, "read: %d bytes from client fd %d (%s) - %d in ibuf", r, c->fd, c->addr_s, c->ibuf_end);
	
	/* parse out rows ending in CR and/or LF and pass them to the handler
	 * without the CRLF (we accept either CR or LF or both, but make sure
	 * to always output CRLF
	 */
	ibuf_end = c->ibuf + c->ibuf_end;
	row_start = c->ibuf;

	for (s = c->ibuf; s < ibuf_end; s++) {
		if (*s == '\r' || *s == '\n') {
			/* found EOL */
			if (s - row_start > 0)
				c->handler(self, c, row_start, s - row_start);
			/* skip the rest of EOL (it might have been zeroed by the handler) */
			while (s < ibuf_end && (*s == '\r' || *s == '\n' || *s == 0))
				s++;
			row_start = s;
		}
	}
	
	if (row_start >= ibuf_end) {
		/* ok, we processed the whole buffer, just mark it empty */
		c->ibuf_end = 0;
	} else if (row_start != c->ibuf) {
		/* ok, we found data... move the buffer contents to the beginning */
		c->ibuf_end = ibuf_end - row_start;
		memmove(c->ibuf, row_start, c->ibuf_end);
	}
	
	return 0;
}

int handle_client_writeable(struct worker_t *self, struct client_t *c)
{
	int r;
	
	if (c->obuf_start == c->obuf_end) {
		/* there is nothing to write any more */
		//hlog(LOG_DEBUG, "write: nothing to write on fd %d", c->fd);
		xpoll_outgoing(self->xp, c->xfd, 0);
		return 0;
	}
	
	r = write(c->fd, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0;

		hlog(LOG_DEBUG, "write: Error from client fd %d (%s): %s", c->fd, c->addr_s, strerror(errno));
		close_client(self, c);
		return -1;
	}
	
	c->obuf_start += r;
	//hlog(LOG_DEBUG, "write: %d bytes to client fd %d (%s) - %d in obuf", r, c->fd, c->addr_s, c->obuf_end - c->obuf_start);
	if (c->obuf_start == c->obuf_end)
		xpoll_outgoing(self->xp, c->xfd, 0);
	
	return 0;
}

int handle_client_event(struct xpoll_t *xp, struct xpoll_fd_t *xfd)
{
	struct worker_t *self = (struct worker_t *)xp->tp;
	struct client_t *c = (struct client_t *)xfd->p;
	
	//hlog(LOG_DEBUG, "handle_client_event(%d): %d", xfd->fd, xfd->result);
	
	if (xfd->result & XP_IN) {
		/* ok, read */
		if (handle_client_readable(self, c) < 0)
			return 0;
	}
	
	if (xfd->result & XP_OUT) {
		/* ah, the client is writable */
		if (handle_client_writeable(self, c) < 0)
			return 0;
	}
	
	return 0;
}

/*
 *	move new clients from the new clients queue to the worker thread
 */

void collect_new_clients(struct worker_t *self)
{
	int pe;
	struct client_t *new_clients, *c;
	
	/* lock the queue */
	if ((pe = pthread_mutex_lock(&self->new_clients_mutex))) {
		hlog(LOG_ERR, "do_accept(worker %d): could not lock new_clients_mutex: %s", self->id, strerror(pe));
		return;
	}
	
	/* quickly grab the new clients to a local variable */
	new_clients = self->new_clients;
	self->new_clients = NULL;
	
	/* unlock */
	if ((pe = pthread_mutex_unlock(&self->new_clients_mutex))) {
		hlog(LOG_ERR, "do_accept(worker %d): could not unlock new_clients_mutex: %s", self->id, strerror(pe));
		/* we'd going to deadlock here... */
		exit(1);
	}
	
	/* move the new clients to the thread local client list */
	while (new_clients) {
		c = new_clients;
		new_clients = c->next;
		
		self->client_count++;
		hlog(LOG_DEBUG, "do_accept(worker %d): got client fd %d", self->id, c->fd);
		c->next = self->clients;
		if (c->next)
			c->next->prevp = &c->next;
		self->clients = c;
		c->prevp = &self->clients;
		
		/* add to polling list */
		c->xfd = xpoll_add(self->xp, c->fd, (void *)c);
		client_printf(self, c, "# Hello %s\r\n", c->addr_s, SERVERID);
	}
	
}

/* 
 *	Send keepalives to client sockets, run this once a second
 *	This watches also obuf_wtime becoming too old, and also about
 *	the number of writes on socket in previous run interval to
 *	auto-adjust socket buffering mode.
 */
void send_keepalives(struct worker_t *self)
{
	struct client_t *c;
	struct tm t;
	char buf[130], *s;
	int len;
	static const char *monthname[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
	time_t w_expire = tick - sock_write_expire;

	// Example message:
	// # javAPRSSrvr 3.12b12 1 Mar 2008 15:11:20 GMT T2FINLAND 85.188.1.32:14580

	sprintf(buf, "# %.40s ",SERVERID);
	s = buf + strlen(buf);

	memset(&t, 0, sizeof(t));
	gmtime_r(&now, &t);
	// s += strftime(s, 40, "%d %b %Y %T GMT", &t);
	// However that depends upon LOCALE, thus following:
	s += sprintf(s, "%d %s %d %02d:%02d:%02d GMT",
		     t.tm_mday, monthname[t.tm_mon], t.tm_year + 1900,
		     t.tm_hour, t.tm_min, t.tm_sec);

	s += sprintf(s, " %s serverIP:serverPORT\r\n", mycall);

	len = (s - buf);

	for (c = self->clients; (c); c = c->next) {

		if (// c->state == CSTATE_UPLINK ||
		    c->state == CSTATE_UPLINKSIM ||
		    c->state == CSTATE_COREPEER)
			continue;
		/* No keepalives on UPLINK or PEER links.. */

		/* Is it time for keepalive ? */
		if (c->keepalive <= tick) {
			int flushlevel = c->obuf_flushsize;
			c->keepalive += keepalive_interval;

			c->obuf_flushsize = 0;
			/* Write out immediately */
			client_write(self, c, buf, len);
			c->obuf_flushsize = flushlevel;
		} else {
			/* just fush if there was anything to write */
			client_write(self, c, buf, 0);
		}
		if (c->obuf_wtime < w_expire) {
			// TOO OLD!  Shutdown the client
			shutdown(c->fd, SHUT_RDWR);
			// Cleanup and close happens by the main loop..
		}
		if (c->obuf_writes > obuf_writes_treshold) {
			// Lots and lots of writes, switch to buffering...

		  if (c->obuf_flushsize == 0)
		    hlog( LOG_DEBUG,"Switch client %p (%s) to buffered writes",
			  c, c->addr_s );

			c->obuf_flushsize = c->obuf_size - 200;
		} else {
		        // Not so much writes, back to "write immediate"
		  if (c->obuf_flushsize != 0)
		    hlog( LOG_DEBUG,"Switch client %p (%s) to unbuffered writes",
			  c, c->addr_s );

			c->obuf_flushsize = 0;
		}
		c->obuf_writes = 0;
	}
}


/*
 *	Worker thread
 */

void worker_thread(struct worker_t *self)
{
	sigset_t sigs_to_block;
	time_t next_keepalive = tick + 2;
	
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
	
	hlog(LOG_INFO, "Worker %d started.", self->id);
	
	while (!self->shutting_down) {
		//hlog(LOG_DEBUG, "Worker %d checking for clients...", self->id);
		if (self->new_clients)
			collect_new_clients(self);
		
		/* poll for incoming traffic */
		xpoll(self->xp, 200);
		
		/* if we have stuff in the local queue, try to flush it and make
		 * it available to the dupecheck thread
		 */
		if (self->pbuf_incoming_local)
			incoming_flush(self);
		
		/* if we have new stuff in the global packet buffer, process it */
		if (*self->pbuf_global_prevp || *self->pbuf_global_dupe_prevp)
			process_outgoing(self);

		/* time of next keepalive broadcast ? */
		if (tick > next_keepalive) {
			send_keepalives(self);
			next_keepalive += keepalive_poll_freq; /* Run them every 2 seconds */
		}
	}
	
	/* stop polling */
	xpoll_close(self->xp);
	self->xp = NULL;
	
	/* close all clients */
	while (self->clients)
		close_client(self, self->clients);
	
	/* workers_stop() will clean up thread-local pbuf pools */
	
	hlog(LOG_DEBUG, "Worker %d shut down.", self->id);
}

/*
 *	Stop workers - runs from accept_thread
 */

void workers_stop(int stop_all)
{
	struct worker_t *w;
	struct pbuf_t *p, *pn;
	int e;
	extern long incoming_count;
	
	while (workers_running > workers_configured || (stop_all && workers_running > 0)) {
		hlog(LOG_DEBUG, "Stopping a worker thread...");
		/* find the last worker thread and shut it down...
		 * could shut down the first one, but to reduce confusion
		 * will shut down the one with the hugest worker id :)
		 *
		 * This could be done even more cleanly by moving the connected
		 * clients to the threads which are left running, but maybe
		 * that'd be way too cool and complicated to implement right now.
		 * It's cool enough to be able to reconfigure at all.
		 */
		w = worker_threads;
		while ((w) && (w->next))
			w = w->next;
		
		w->shutting_down = 1;
		if ((e = pthread_join(w->th, NULL)))
			hlog(LOG_ERR, "Could not pthread_join worker %d: %s", w->id, strerror(e));
		else
			hlog(LOG_INFO, "Worker %d has terminated.", w->id);



		for (p = w->pbuf_free_small; p; p = pn) {
			pn = p->next;
			pbuf_free(NULL, p);
		}
		for (p = w->pbuf_free_large; p; p = pn) {
			pn = p->next;
			pbuf_free(NULL, p);
		}
		for (p = w->pbuf_free_huge; p; p = pn) {
			pn = p->next;
			pbuf_free(NULL, p);
		}

		*(w->prevp) = NULL;
		hfree(w);
		
		workers_running--;
	}
	hlog(LOG_INFO, "workers; incount=%ld", incoming_count);
	
}

/*
 *	Start workers - runs from accept_thread
 */

void workers_start(void)
{
	int i;
	struct worker_t * volatile w;
 	struct worker_t **prevp;
	
	workers_stop(0);

	while (workers_running < workers_configured) {
		hlog(LOG_DEBUG, "Starting a worker thread...");
		i = 0;
		prevp = &worker_threads;
		w = worker_threads;
		while (w) {
			prevp = &w->next;
			w = w->next;
			i++;
		}
		
		w = hmalloc(sizeof(*w));
		*prevp = w;
		memset(w, 0, sizeof(*w));

		w->prevp = prevp;

		w->id = i;
		pthread_mutex_init(&w->new_clients_mutex, NULL);
		w->xp = xpoll_init((void *)w, &handle_client_event);
		
		w->pbuf_incoming_local = NULL;
		w->pbuf_incoming_local_last = &w->pbuf_incoming_local;
		
		w->pbuf_incoming      = NULL;
		w->pbuf_incoming_last = &w->pbuf_incoming;
		pthread_mutex_init(&w->pbuf_incoming_mutex, NULL);
		
		w->pbuf_global_prevp      = pbuf_global_prevp;
		w->pbuf_global_dupe_prevp = pbuf_global_dupe_prevp;

		/* start the worker thread */
		if (pthread_create(&w->th, NULL, (void *)worker_thread, w))
			perror("pthread_create failed for worker_thread");
		
		workers_running++;
	}
}

