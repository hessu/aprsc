
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

struct listen_t {
	struct listen_t *next;
	struct listen_t **prevp;
	
	struct sockaddr_in sin;
	socklen_t addr_len;
	int fd;
	
	char *addr_s;
} *listen_list = NULL;

pthread_mutex_t mt_servercount = PTHREAD_MUTEX_INITIALIZER;

int accept_reconfiguring = 0;
int accept_shutting_down = 0;

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
	
	if (bind(f, (struct sockaddr *)&l->sin, sizeof(l->sin))) {
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
	char eb[80];
	int opened = 0;
	
	for (lc = listen_config; (lc); lc = lc->next) {
		l = hmalloc(sizeof(*l));
		l->fd = -1;
		l->sin.sin_family = AF_INET;
		l->sin.sin_port = htons(lc->port);
		
		if (lc->host) {
			if (!(he = gethostbyname(lc->host))) {
				h_strerror(h_errno, eb, sizeof(eb));
				hlog(LOG_CRIT, "Listen: Could not resolve \"%s\": %s - not listening on port %d\n", lc->host, eb, lc->port);
				hfree(l);
				continue;
			}
			memcpy(&l->sin.sin_addr.s_addr, he->h_addr_list[0], he->h_length);
		} else {
			l->sin.sin_addr.s_addr = INADDR_ANY;
		}
		
		aptoa(l->sin.sin_addr, ntohs(l->sin.sin_port), eb, sizeof(eb));
		l->addr_s = hstrdup(eb);
		
		if (open_tcp_listener(l) >= 0) {
			opened++;
			hlog(LOG_DEBUG, "... ok, bound");
		} else {
			hlog(LOG_DEBUG, "... failed");
			hfree(l->addr_s);
			hfree(l);
			continue;
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
		if (l->fd >= 0)
			close(l->fd);
		hfree(l->addr_s);
		hfree(l);
	}
}

/*
 *	Accept a single connection
 */

struct client_t *do_accept(struct listen_t *l)
{
	char eb[80];
	struct client_t *c = hmalloc(sizeof(*c));
	memset((void *)c, 0, sizeof(*c));
	c->addr_len = sizeof(c->addr);
	
	if ((c->fd = accept(l->fd, &c->addr, &c->addr_len)) < 0) {
		int e = errno;
		switch (e) {
			case EAGAIN:
			case ENETDOWN:
			case EPROTO:
			case ENOPROTOOPT:
			case EHOSTDOWN:
			case ENONET:
			case EHOSTUNREACH:
			case EOPNOTSUPP:
			case ENETUNREACH:
			case ECONNABORTED:
			case ECONNRESET:
			case ENOBUFS:
			case ETIMEDOUT:
			case EISCONN:
			case ENOTCONN:
				hlog(LOG_ERR, "accept() failed: %s (continuing)", strerror(e));
				hfree(c);
				return NULL;
			default:
				hlog(LOG_CRIT, "accept() failed: %s (giving up)", strerror(e));
				hfree(c);
				exit(1);
		}
	}
	
	struct sockaddr_in *sin = (struct sockaddr_in *)&c->addr;
	aptoa(sin->sin_addr, ntohs(sin->sin_port), eb, sizeof(eb));
	c->addr_s = hstrdup(eb);
	c->ibuf_size = ibuf_size;
	c->ibuf = hmalloc(c->ibuf_size);
	c->obuf_size = obuf_size;
	c->obuf = hmalloc(c->obuf_size);
	hlog(LOG_DEBUG, "%s - Accepted connection on fd %d from %s", l->addr_s, c->fd, eb);
	
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
	close(c->fd);
	hfree(c->ibuf);
	hfree(c);
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
			if ((listen_n = open_listeners()) <= 0)
				hlog(LOG_CRIT, "Failed to listen on any ports.");
			
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

