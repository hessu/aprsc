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

struct listen_t {
	struct listen_t *next;
	struct listen_t **prevp;
	
	union sockaddr_u sa;
	socklen_t addr_len;
	int fd;
	
	char *addr_s;
	char *filters[10];
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
	
	if ((f = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		hlog(LOG_CRIT, "socket(): %s\n", strerror(errno));
		return -1;
	}
	
	arg = 1;
	setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg));
	
	if (bind(f, (struct sockaddr *)&l->sa, l->addr_len)) {
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
	struct hostent *he;
	char eb[80], *s;
	int opened = 0, i;
	
	for (lc = listen_config; (lc); lc = lc->next) {
		l = listener_alloc();

		l->sa.si.sin_family = AF_INET;
		l->sa.si.sin_port = htons(lc->port);
		l->addr_len = sizeof(l->sa.si);

		if (lc->host) {
			if (!(he = gethostbyname(lc->host))) {
				h_strerror(h_errno, eb, sizeof(eb));
				hlog(LOG_CRIT, "Listen: Could not resolve \"%s\": %s - not listening on port %d\n", lc->host, eb, lc->port);
				listener_free(l);
				continue;
			}
			memcpy(&l->sa.si.sin_addr.s_addr, he->h_addr_list[0], he->h_length);
		} else {
			l->sa.si.sin_addr.s_addr = INADDR_ANY;
		}
		
		eb[0] = '[';
		inet_ntop(l->sa.sa.sa_family, &l->sa.si.sin_addr, eb+1, sizeof(eb)-1);
		s = eb + strlen(eb);
		sprintf(s, "]:%d", ntohs(((l->sa.sa.sa_family == AF_INET) ? l->sa.si.sin_port : l->sa.si6.sin6_port)));

		l->addr_s = hstrdup(eb);
		
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

		hlog(LOG_DEBUG, "... adding to listened sockets");
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
	struct client_t *c;
	union sockaddr_u    sa;
	socklen_t addr_len = sizeof(sa);
	char eb[200];
	char *s;
	
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
	
	c = client_alloc();
	c->fd    = fd;
	c->addr  = sa;
	c->state = CSTATE_LOGIN;

	eb[0] = '[';
	inet_ntop(sa.sa.sa_family, &sa.si.sin_addr, eb+1, sizeof(eb)-1);
	s = eb + strlen(eb);
	sprintf(s, "]:%d", ntohs((sa.sa.sa_family == AF_INET) ? sa.si.sin_port : sa.si6.sin6_port));

	c->addr_s = hstrdup(eb);
	hlog(LOG_DEBUG, "%s - Accepted connection on fd %d from %s", l->addr_s, c->fd, eb);
	
	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
		if (l->filters[i])
			filter_parse(c, l->filters[i]);
	}

	/* set non-blocking mode */
	if (fcntl(c->fd, F_SETFL, O_NONBLOCK)) {
		hlog(LOG_ERR, "%s - Failed to set non-blocking mode on socket: %s", l->addr_s, strerror(errno));
		goto err;
	}
	
	/* find the worker with least clients...
	 * This isn't strictly accurate, since the threads could change their
	 * client counts during scanning, but we don't really care if the load distribution
	 * is _exactly_ fair.
	 */
	
	struct worker_t *w;
	struct worker_t *wc = worker_threads;
	int client_min = -1;
	for (w = worker_threads; (w); w = w->next)
		if (w->client_count < client_min || client_min == -1) {
			wc = w;
			client_min = w->client_count;
		}
	
	/* ok, found it... lock the new client queue */
	hlog(LOG_DEBUG, "... passing to thread %d with %d users", wc->id, wc->client_count);
	int pe;
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
			
			/* stop the dupechecking thread (if it runs) while adjusting
			 * the amount of workers... it walks the worker list.
			 */
			dupecheck_stop();
			workers_start();
			dupecheck_start();
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
	close_listeners();
	dupecheck_stop();
	workers_stop(1);
}

