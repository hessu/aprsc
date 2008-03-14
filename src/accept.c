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
 *	accept.c: the connection accepting thread
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "accept.h"
#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "netlib.h"
#include "worker.h"
#include "dupecheck.h"
#include "filter.h"
#include "login.h"
#include "uplink.h"

extern int uplink_simulator;

struct listen_t {
	struct listen_t *next;
	struct listen_t **prevp;
	
	struct addrinfo *ai;
	int fd;
	
	char *name;
	char *addr_s;
	char *filters[10]; // up to 10 filter definitions
} *listen_list = NULL;

pthread_mutex_t mt_servercount = PTHREAD_MUTEX_INITIALIZER;

int accept_reconfiguring = 0;
int accept_shutting_down = 0;


/* structure allocator/free */

struct listen_t *listener_alloc(void)
{
	struct listen_t *l = hmalloc(sizeof(*l));
	memset((void *)l, 0, sizeof(*l));
	l->fd = -1;

	return l;
}

void listener_free(struct listen_t *l)
{
	int i;

	if (l->fd >= 0)	close(l->fd);
	if (l->addr_s)	hfree(l->addr_s);
	if (l->name)	hfree(l->name);

	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i)
		if (l->filters[i])
			hfree(l->filters[i]);

	hfree(l);
}


/*
 *	signal handler
 */
 
int accept_sighandler(int signum)
{
	switch (signum) {
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)accept_sighandler);	/* restore handler */
	return 0;
}

/*
 *	Open the TCP listening socket
 */

int open_tcp_listener(struct listen_t *l)
{
	int arg;
	int f;
	
	hlog(LOG_INFO, "Binding listening TCP socket: %s", l->addr_s);
	
	if ((f = socket(l->ai->ai_family, l->ai->ai_socktype, l->ai->ai_protocol)) < 0) {
		hlog(LOG_CRIT, "socket(): %s\n", strerror(errno));
		return -1;
	}
	
	arg = 1;
#ifdef SO_REUSEPORT
	setsockopt(f, SOL_SOCKET, SO_REUSEPORT, (char *)&arg, sizeof(arg));
#else
	setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg));
#endif
	
	if (bind(f, l->ai->ai_addr, l->ai->ai_addrlen)) {
		hlog(LOG_CRIT, "bind(%s): %s", l->addr_s, strerror(errno));
		close(f);
		return -1;
	}
	
	if (listen(f, SOMAXCONN)) {
		hlog(LOG_CRIT, "listen(%s) failed: %s", l->addr_s, strerror(errno));
		close(f);
		return -1;
	}
	
	l->fd = f;
	
	return f;
}

int open_listeners(void)
{
	struct listen_config_t *lc;
	struct listen_t *l;
	char eb[120], *s;
	char sbuf[20];
	int opened = 0, i;
	
	for (lc = listen_config; (lc); lc = lc->next) {
		l = listener_alloc();

		/* Pick first of the AIs for this listen definition */
		
		eb[0] = '[';
		eb[1] = 0;
		*sbuf = 0;

		l->ai = lc->ai;

		getnameinfo(l->ai->ai_addr, l->ai->ai_addrlen,
			    eb+1, sizeof(eb)-1, sbuf, sizeof(sbuf), NI_NUMERICHOST|NI_NUMERICSERV);
		s = eb + strlen(eb);
		sprintf(s, "]:%s", sbuf);

		l->addr_s = hstrdup(eb);
		l->name   = hstrdup(lc->name);
		
		if (open_tcp_listener(l) >= 0) {
			opened++;
			hlog(LOG_DEBUG, "... ok, bound");
		} else {
			hlog(LOG_DEBUG, "... failed");
			listener_free(l);
			continue;
		}

		/* Copy filter definitions */
		for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
			if (i < (sizeof(lc->filters)/sizeof(lc->filters[0])))
				l->filters[i] = (lc->filters[i]) ? hstrdup(lc->filters[i]) : NULL;
			else
				l->filters[i] = NULL;
		}

		hlog(LOG_DEBUG, "... adding %s to listened sockets", eb);
		// put (first) in the list of listening sockets
		l->next = listen_list;
		l->prevp = &listen_list;
		if (listen_list)
			listen_list->prevp = &l->next;
		listen_list = l;
	}
	
	return opened;
}

void close_listeners(void)
{
	if (!listen_list)
		return;
		
	hlog(LOG_DEBUG, "Closing listening sockets....");
	struct listen_t *l;
	while (listen_list) {
		l = listen_list;
		listen_list = listen_list->next;

		listener_free(l);
	}
}


/*
 *	Accept a single connection
 */

struct client_t *do_accept(struct listen_t *l)
{
	int fd, i;
	int pe;
	struct client_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len = sizeof(sa);
	char eb[200];
	char sbuf[20];
	char *s;
	static int next_receiving_worker = 0;
	struct worker_t *w;
	struct worker_t *wc;
	int client_min = -1;
	
	if ((fd = accept(l->fd, (struct sockaddr*)&sa, &addr_len)) < 0) {
		int e = errno;
		switch (e) {
			/* Errors reporting really bad internal (programming) bugs */
			case EBADF:
			case EINVAL:
#ifdef ENOTSOCK
			case ENOTSOCK: /* Not a socket */
#endif
#ifdef EOPNOTSUPP
			case EOPNOTSUPP: /* Not a SOCK_STREAM */
#endif
#ifdef ESOCKTNOSUPPORT
			case ESOCKTNOSUPPORT: /* Linux errors ? */
#endif
#ifdef EPROTONOSUPPORT
			case EPROTONOSUPPORT: /* Linux errors ? */
#endif

				hlog(LOG_CRIT, "accept() failed: %s (giving up)", strerror(e));
				exit(1); // ABORT with core-dump ??

				break;

			/* Errors reporting system internal/external glitches */
			default:
				hlog(LOG_ERR, "accept() failed: %s (continuing)", strerror(e));
				return NULL;
		}
	}
	
	eb[0] = '[';
	eb[1] = 0;
	*sbuf = 0;

	getnameinfo((struct sockaddr *)&sa, addr_len,
		    eb+1, sizeof(eb)-1, sbuf, sizeof(sbuf), NI_NUMERICHOST|NI_NUMERICSERV);
	s = eb + strlen(eb);
	sprintf(s, "]:%s", sbuf);

	c = client_alloc();
	c->fd    = fd;
	c->addr  = sa;
	c->state = CSTATE_LOGIN;
	c->addr_s = hstrdup(eb);
	c->keepalive = tick;
	/* use the default login handler */
	c->handler = &login_handler;

#if WBUF_ADJUSTER
	{
	  int len, arg;
	  /* Ask system's idea about what the sndbuf size is.  */
	  len = sizeof(arg);
	  i = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &arg, &len);
	  if (i == 0)
	    c->wbuf_size = arg;
	  else
	    c->wbuf_size = 8192; // default is syscall fails
	}
#endif

	if (strcmp(l->name,"uplinksim") == 0) {
	  // uplink simulator
	  c->state = CSTATE_UPLINKSIM;
	  c->handler = uplink_login_handler;
	  uplink_simulator = 1;
	}

	hlog(LOG_DEBUG, "%s - Accepted connection on fd %d from %s", l->addr_s, c->fd, eb);
	
	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
		if (l->filters[i])
			filter_parse(c, l->filters[i], 0); /* system filters */
	}
	
	/* set non-blocking mode */
	if (fcntl(c->fd, F_SETFL, O_NONBLOCK)) {
		hlog(LOG_ERR, "%s - Failed to set non-blocking mode on socket: %s", l->addr_s, strerror(errno));
		goto err;
	}

#if 1
	/* Use simple round-robin on client feeding.  Least clients is
	 * quite attractive idea, but when clients arrive at huge bursts
	 * they tend to move in big bunches, and it takes quite some while
	 * before the worker updates its client-counters.
	 */
	for (i = 0, w = worker_threads; w ;  w = w->next, ++i) {
	  if ( i >= next_receiving_worker) break;
	}
	wc = w;
	if (! w) {
	  wc = worker_threads;       // ran out of the worker chain, back to the first..
	  next_receiving_worker = 0; // and reset the index too
	}
	// in every case, increment the next receiver index for the next call.
	++next_receiving_worker;

#else
	/* find the worker with least clients...
	 * This isn't strictly accurate, since the threads could change their
	 * client counts during scanning, but we don't really care if the load distribution
	 * is _exactly_ fair.
	 */
	
	for (wc = w = worker_threads; (w); w = w->next)
		if (w->client_count < client_min || client_min == -1) {
			wc = w;
			client_min = w->client_count;
		}
#endif

	/* ok, found it... lock the new client queue */
	hlog(LOG_DEBUG, "... passing to thread %d with %d users", wc->id, wc->client_count);

	if ((pe = pthread_mutex_lock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "do_accept(): could not lock new_clients_mutex: %s", strerror(pe));
		goto err;
	}
	/* push the client in the worker's queue */
	c->next = wc->new_clients;
	c->prevp = &wc->new_clients;
	if (c->next)
		c->next->prevp = &c->next;
	wc->new_clients = c;
	/* unlock the queue */
	if ((pe = pthread_mutex_unlock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "do_accept(): could not unlock new_clients_mutex: %s", strerror(pe));
		goto err;
	}
	
	return c;
	
err:
	client_free(c);
	return 0;
}

/*
 *	Accept thread
 */

void accept_thread(void *asdf)
{
	sigset_t sigs_to_block;
	int e, n;
	struct pollfd *acceptpfd = NULL;
	int listen_n = 0;
	struct listen_t *l;

	pthreads_profiling_reset("accept");
	
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
	
	/* start the accept thread, which will start server threads */
	hlog(LOG_INFO, "Accept_thread starting...");
	
	accept_reconfiguring = 1;
	while (!accept_shutting_down) {
		if (accept_reconfiguring) {
			accept_reconfiguring = 0;
			if (listen_n)
				close_listeners();
			
			/* start listening on the sockets */
			if ((listen_n = open_listeners()) <= 0) {
				hlog(LOG_CRIT, "Failed to listen on any ports.");
				exit(2);
			}
			
			hlog(LOG_DEBUG, "Generating polling list...");
			acceptpfd = hmalloc(listen_n * sizeof(*acceptpfd));
			n = 0;
			for (l = listen_list; (l); l = l->next) {
				hlog(LOG_DEBUG, "... %d: fd %d (%s)", n, l->fd, l->addr_s);
				acceptpfd[n].fd = l->fd;
				acceptpfd[n].events = POLLIN|POLLPRI|POLLERR|POLLHUP;
				n++;
			}
			hlog(LOG_INFO, "Accept thread ready.");
			
			/* stop the dupechecking and uplink threads while adjusting
			 * the amount of workers... they walk the worker list, and
			 * might get confused when workers are stopped or started.
			 */
			uplink_stop();
			dupecheck_stop();
			workers_start();
			dupecheck_start();
			uplink_start();
		}
		
		/* check for new connections */
		e = poll(acceptpfd, listen_n, 1000);
		if (e == 0)
			continue;
		if (e < 0) {
			if (errno == EINTR)
				continue;
			hlog(LOG_ERR, "poll() on accept failed: %s (continuing)", strerror(errno));
			continue;
		}
		
		/* now, which socket was that on? */
		l = listen_list;
		for (n = 0; n < listen_n; n++) {
			if (!(l) || l->fd != acceptpfd[n].fd) {
				hlog(LOG_CRIT, "accept_thread: polling list and listener list do mot match!");
				exit(1);
			}
			if (acceptpfd[n].revents)
				do_accept(l); /* accept a single connection */
			l = l->next;
		}
	}
	
	hlog(LOG_DEBUG, "Accept thread shutting down listening sockets and worker threads...");
	uplink_stop();
	close_listeners();
	dupecheck_stop();
	workers_stop(1);
	hfree(acceptpfd);
}

