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

#include "worker.h"

#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "login.h"
#include "uplink.h"
#include "incoming.h"
#include "outgoing.h"
#include "filter.h"
#include "dupecheck.h"

time_t now;	/* current time, updated by the main thread, MAY be spun around by the simulator */
time_t tick;	/* real monotonous clock, may or may not be wallclock */

extern int ibuf_size;

struct worker_t *worker_threads;
struct client_t *udpclient;	/* single listening/receiving UDP client socket */
struct client_t *udppeer;	/* single listening/receiving UDP peer socket */

int workers_running;
int sock_write_expire  = 60;    /* 60 seconds OK ?       */
int keepalive_interval = 20;    /* 20 seconds for individual socket, NOT all in sync! */
int keepalive_poll_freq = 2;	/* keepalive analysis scan interval */
int obuf_writes_treshold = 5;	/* This many writes per keepalive scan interval switch socket
				   output to buffered. */

/* global packet buffer */
rwlock_t pbuf_global_rwlock = RWL_INITIALIZER;
struct pbuf_t  *pbuf_global       = NULL;
struct pbuf_t  *pbuf_global_last  = NULL;
struct pbuf_t **pbuf_global_prevp = &pbuf_global;
struct pbuf_t  *pbuf_global_dupe       = NULL;
struct pbuf_t  *pbuf_global_dupe_last  = NULL;
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
	      self->id, ( (c->flags & (CLFLAGS_UPLINKPORT|CLFLAGS_UPLINKSIM))
			  ? "client":"uplink" ), c->fd, c->addr_s);

	/* close */
	if (c->fd >= 0) {
		close(c->fd);
	
		/* remove from polling list */
		if (self->xp)
			xpoll_remove(self->xp, c->xfd);
	}

	c->fd = -1;

	
	/* link the list together over this node */
	if (c->next)
		c->next->prevp = c->prevp;
	*c->prevp = c->next;

	/* if this happens to be the uplink, tell the uplink module that the
	 * connection has gone away
	 */
	if (c->flags & CLFLAGS_UPLINKPORT)
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

	if (c->udp_port && udpclient && len > 0) {
		int i;
		i = sendto( udpclient->fd, p, len, MSG_DONTWAIT,
			    (struct sockaddr *)&c->udpaddr, c->udpaddrlen );

		hlog(LOG_DEBUG, "UDP to client port %d, sendto rc=%d, errno %s",
		     c->udp_port, i, strerror(errno));
		// FIXME: UDP write statistics !

		if (*p != '#') // Send keepalive also through the tcp control socket..
			return i;
	}
	if (c->state == CSTATE_UDP)
		return 0;

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
			if (i < 0 && (e == EPIPE)) {
				/* Remote socket closed.. */
				hlog(LOG_DEBUG, "client_write(%s) fails/2; %s", c->addr_s, strerror(e));
				// WARNING: This also destroys the client object!
				close_client(self, c);
				return -9;
			}
			if (i < 0 && (e == EAGAIN || e == EWOULDBLOCK)) {
			  // FIXME: WHAT TO DO ?
			  // USE WBUF_ADJUSTER ?
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
			  hlog(LOG_DEBUG, "client_write(%s) fails/1; %s", c->addr_s, strerror(e));
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
		if (i < 0 && (e == EPIPE)) {
			/* Remote socket closed.. */
			hlog(LOG_DEBUG, "client_write(%s) fails/2; %s", c->addr_s, strerror(e));
			// WARNING: This also destroys the client object!
			close_client(self, c);
			return -9;
		}
		if (i < 0 && (e == EAGAIN || e == EWOULDBLOCK)) {
		  // FIXME: WHAT TO DO ?
		  // USE WBUF_ADJUSTER ?
		  /* tell the poller that we have outgoing data */
		  xpoll_outgoing(self->xp, c->xfd, 1);
		  return 0; // but could not write it at this time..
		}
		if (i < 0 && len != 0) {
			hlog(LOG_DEBUG, "client_write(%s) fails/2; %s", c->addr_s, strerror(e));
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
int client_bad_filter_notify(struct worker_t *self, struct client_t *c, const char *filt)
{
	if (!c->warned) {
		c->warned = 1;
		return client_printf(self, c, "# Warning: Bad filter: %s\r\n", filt);
	}
	return 0;
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

	if (c->fd < 0) {
		hlog(LOG_DEBUG, "client no longer alive, closing (%s)", c->fd, c->addr_s);
		close_client(self, c);
		return -1;
	}

	if (c->state == CSTATE_UDP) {
		union sockaddr_u sa;
		socklen_t fromlen = sizeof(sa);
		int freelen = c->ibuf_size - c->ibuf_end;
		// FIXME: verify: freelen > X (512 ?)
		r = recvfrom( c->fd, c->ibuf + c->ibuf_end, freelen, MSG_DONTWAIT,
			      (struct sockaddr *) & sa, & fromlen );

		// FIXME: received via UDP, now verify source IP address and port!
		// FIXME: verify that received packet had CRLF at the tail!
	} else {
		r = read(c->fd, c->ibuf + c->ibuf_end, c->ibuf_size - c->ibuf_end - 1);
	}
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
			  /* NOTE: handler call CAN destroy the c-> object ! */
			  if (c->handler(self, c, row_start, s - row_start) < 0)
			    return -1;
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
		c->obuf_start = c->obuf_end = 0;
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
	if (c->obuf_start == c->obuf_end) {
		xpoll_outgoing(self->xp, c->xfd, 0);
		c->obuf_start = c->obuf_end = 0;
	}
	
	return 0;
}

int handle_client_event(struct xpoll_t *xp, struct xpoll_fd_t *xfd)
{
	struct worker_t *self = (struct worker_t *)xp->tp;
	struct client_t *c    = (struct client_t *)xfd->p;
	
	//hlog(LOG_DEBUG, "handle_client_event(%d): %d", xfd->fd, xfd->result);

	if (xfd->result & XP_OUT) {  /* priorize doing output */
		/* ah, the client is writable */
		if (handle_client_writeable(self, c) < 0)
			return 0;
	}

	if (xfd->result & XP_IN) {  /* .. before doing input */
		/* ok, read */
		if (handle_client_readable(self, c) < 0)
			return 0;
	}
	
	return 0;
}

/*
 *	move new clients from the new clients queue to the worker thread
 */

void collect_new_clients(struct worker_t *self)
{
	int pe, n;
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
	n = self->xp->pollfd_used;
	while (new_clients) {
		c = new_clients;
		new_clients = c->next;
		
		self->client_count++;
		// hlog(LOG_DEBUG, "do_accept(worker %d): got client fd %d", self->id, c->fd);
		c->next = self->clients;
		if (c->next)
			c->next->prevp = &c->next;
		self->clients = c;
		c->prevp = &self->clients;
		
		/* add to polling list */
		c->xfd = xpoll_add(self->xp, c->fd, (void *)c);

		/* the new client may end up destroyed right away, never mind it here... */
		client_printf(self, c, "# Hello %s\r\n", c->addr_s, SERVERID);
	}
	hlog( LOG_DEBUG, "Worker %d accepted %d new clients, now total %d",
	      self->id, self->xp->pollfd_used - n, self->xp->pollfd_used );
}

/* 
 *	Send keepalives to client sockets, run this once a second
 *	This watches also obuf_wtime becoming too old, and also about
 *	the number of writes on socket in previous run interval to
 *	auto-adjust socket buffering mode.
 */
void send_keepalives(struct worker_t *self)
{
	struct client_t *c, *cnext;
	struct tm t;
	char buf[130], *s;
	int len, rc;
	static const char *monthname[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
	time_t w_expire    = tick - sock_write_expire;
	time_t w_keepalive = tick - keepalive_interval;

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

	for (c = self->clients; (c); c = cnext) {
		// the  c  may get destroyed from underneath of ourselves!
		cnext = c->next;

		if ( c->flags & (CLFLAGS_UPLINKSIM|CLFLAGS_UPLINKPORT) ||
		     c->state == CSTATE_COREPEER )
			continue;
		/* No keepalives on UPLINK or PEER links.. */

		/* Is it time for keepalive ? */
		if (c->keepalive <= tick && c->obuf_wtime < w_keepalive) {
			int flushlevel = c->obuf_flushsize;
			c->keepalive = tick + keepalive_interval;

			c->obuf_flushsize = 0;
			/* Write out immediately */
			rc = client_write(self, c, buf, len);
			if (rc < -2) continue; // destroyed
			c->obuf_flushsize = flushlevel;
		} else {
			/* just fush if there was anything to write */
			rc = client_write(self, c, buf, 0);
			if (rc < -2) continue; // destroyed..
		}
		if (c->obuf_wtime < w_expire && c->state != CSTATE_UDP) {
			// TOO OLD!  Shutdown the client
			hlog( LOG_DEBUG,"Closing client %p fd %d (%s) due to obuf wtime timeout",
			      c, c->fd, c->addr_s );
			close_client(self, c);
			continue;
		}
		if (c->obuf_writes > obuf_writes_treshold) {
			// Lots and lots of writes, switch to buffering...

		  if (c->obuf_flushsize == 0)
		    // hlog( LOG_DEBUG,"Switch client %p fd %d (%s) to buffered writes", c, c->fd, c->addr_s );

			c->obuf_flushsize = c->obuf_size - 200;
		} else {
		        // Not so much writes, back to "write immediate"
		  if (c->obuf_flushsize != 0)
		    // hlog( LOG_DEBUG,"Switch client %p fd %d (%s) to unbuffered writes", c, c->fd, c->addr_s );

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
	char myname[20];
	struct pbuf_t *p, *pn;
	time_t next_lag_query = tick + 10;
	time_t t1, t2, t3, t4, t5, t6, t7;

	sprintf(myname,"worker %d", self->id);
	pthreads_profiling_reset(myname);

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
	
	hlog(LOG_DEBUG, "Worker %d started.", self->id);

	if (self->id == 1 && udpclient) {  /* Worker ZERO picks up the udpclient, if any */
	  int pe;
	  /* lock the queue */
	  if ((pe = pthread_mutex_lock(&self->new_clients_mutex))) {
	    hlog(LOG_ERR, "collect_new_clients(worker %d): could not lock new_clients_mutex: %s", self->id, strerror(pe));
	    return;
	  }
	
	  /* Put the udp-client on head of the new client list.. */
	  udpclient->next = self->new_clients;
	  self->new_clients = udpclient;

	  /* unlock */
	  if ((pe = pthread_mutex_unlock(&self->new_clients_mutex))) {
	    hlog(LOG_ERR, "collect_new_clients(worker %d): could not unlock new_clients_mutex: %s", self->id, strerror(pe));
	    /* we'd going to deadlock here... */
	    exit(1);
	  }
	}

	while (!self->shutting_down) {
		//hlog(LOG_DEBUG, "Worker %d checking for clients...", self->id);
		t1 = tick;

		/* if we have new stuff in the global packet buffer, process it */
		if (*self->pbuf_global_prevp || *self->pbuf_global_dupe_prevp)
			process_outgoing(self);

		t2 = tick;

		// TODO: calculate different delay based on outgoing lag ?
		/* poll for incoming traffic */
		xpoll(self->xp, 200);
		
		/* if we have stuff in the local queue, try to flush it and make
		 * it available to the dupecheck thread
		 */
		t3 = tick;

		if (self->pbuf_incoming_local)
			incoming_flush(self);
		
		t4 = tick;

		if (self->new_clients)
			collect_new_clients(self);

		t5 = tick;

		/* time of next keepalive broadcast ? */
		if (tick > next_keepalive) {
			next_keepalive += keepalive_poll_freq; /* Run them every 2 seconds */
			send_keepalives(self);
		}
		t6 = tick;

		if (tick > next_lag_query) {
			int lag, lag1, lag2;
			next_lag_query += 10; // every 10 seconds..
			lag = outgoing_lag_report(self, &lag1, &lag2);
			hlog(LOG_DEBUG, "Thread %d  pbuf lag %d,  dupelag %d", self->id, lag1, lag2);
		}
		t7 = tick;

		if (t7-t1 > 1) // only report if the delay is over 1 seconds.  they are a LOT rarer
		  hlog( LOG_DEBUG, "Worker thread %d loop step delays:  dt2: %d  dt3: %d  dt4: %d  dt5: %d  dt6: %d  dt7: %d",
			self->id, t2-t1, t3-t1, t4-t1, t5-t1, t6-t1, t7-t1 );
	}
	
	/* stop polling */
	xpoll_close(self->xp);
	self->xp = NULL;
	
	/* close all clients */
	while (self->clients)
		close_client(self, self->clients);
	
	/* clean up thread-local pbuf pools */

	for (p = self->pbuf_free_small; p; p = pn) {
		pn = p->next;
		pbuf_free(NULL, p); // free to global pool
	}
	for (p = self->pbuf_free_large; p; p = pn) {
		pn = p->next;
		pbuf_free(NULL, p); // free to global pool
	}
	for (p = self->pbuf_free_huge; p; p = pn) {
		pn = p->next;
		pbuf_free(NULL, p); // free to global pool
	}

	hlog(LOG_DEBUG, "Worker %d shut down.", self->id);
}

/*
 *	Stop workers - runs from accept_thread
 */

void workers_stop(int stop_all)
{
	struct worker_t *w;
	int e;
	int stopped = 0;
	extern long incoming_count;
	
	hlog(LOG_INFO, "Stopping %d worker threads...",
		(stop_all) ? workers_running : workers_configured - workers_running);
	while (workers_running > workers_configured || (stop_all && workers_running > 0)) {
		hlog(LOG_DEBUG, "Stopping a worker thread...");
		/* find the last worker thread and shut it down...
		 * could shut down the first one, but to reduce confusion
		 * will shut down the one with the largest worker id :)
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
		if ((e = pthread_join(w->th, NULL))) {
			hlog(LOG_ERR, "Could not pthread_join worker %d: %s", w->id, strerror(e));
		} else {
			hlog(LOG_DEBUG, "Worker %d has terminated.", w->id);
			stopped++;
		}

		*(w->prevp) = NULL;
		hfree(w);
		
		workers_running--;
	}
	hlog(LOG_INFO, "Stopped %d worker threads. (incoming_count=%ld)", stopped, incoming_count);
	
}

/*
 *	Start workers - runs from accept_thread
 */

void workers_start(void)
{
	int i;
	struct worker_t * volatile w;
 	struct worker_t **prevp;
	
	if (workers_running)
		workers_stop(0);
	
	hlog(LOG_INFO, "Starting %d worker threads (configured: %d)...",
		workers_configured - workers_running, workers_configured);
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

