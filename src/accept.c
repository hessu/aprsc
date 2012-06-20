/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
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
#include "incoming.h" /* incoming_handler prototype */
#include "uplink.h"

extern int uplink_simulator;

struct listen_t {
	struct listen_t *next;
	struct listen_t **prevp;
	
	int fd;
	int clientflags;
	int portnum;
	int clients_max;
	struct client_udp_t *udp;
	struct portaccount_t *portaccount;
	struct acl_t *acl;

	char *name;
	char *addr_s;
	char *filters[10]; // up to 10 filter definitions
};

static struct listen_t *listen_list;

//  pthread_mutex_t mt_servercount = PTHREAD_MUTEX_INITIALIZER;

int accept_shutting_down;
int accept_reconfiguring;


/* structure allocator/free */

static struct listen_t *listener_alloc(void)
{
	struct listen_t *l = hmalloc(sizeof(*l));
	memset( l, 0, sizeof(*l) );
	l->fd = -1;

	return l;
}

static void listener_free(struct listen_t *l)
{
	int i;

	if (l->udp) {
		l->udp->configured = 0;
		l->fd = -1;

		client_udp_free(l->udp);
	}

	port_accounter_drop(l->portaccount); /* The last reference, perhaps. */

	if (l->fd >= 0)	close(l->fd);
	if (l->addr_s)	hfree(l->addr_s);
	if (l->name)	hfree(l->name);

	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i)
		if (l->filters[i])
			hfree(l->filters[i]);
	
	if (l->acl)
		acl_free(l->acl);
	
	hfree(l);
}


#if 0
/*
 *	signal handler
 */
 
static int accept_sighandler(int signum)
{
	switch (signum) {
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)accept_sighandler);	/* restore handler */
	return 0;
}
#endif

/*
 *	Open the TCP/SCTP listening socket
 */

static int open_tcp_listener(struct listen_t *l, const struct addrinfo *ai)
{
	int arg;
	int f;
	
	hlog(LOG_INFO, "Binding listening TCP socket: %s", l->addr_s);
	
	if ((f = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
		hlog(LOG_CRIT, "socket(): %s\n", strerror(errno));
		return -1;
	}
	
	arg = 1;
	setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg));
#ifdef SO_REUSEPORT
	setsockopt(f, SOL_SOCKET, SO_REUSEPORT, (char *)&arg, sizeof(arg));
#endif
	
	if (bind(f, ai->ai_addr, ai->ai_addrlen)) {
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

/*
 *	Open the UDP receiving socket
 */

static int open_udp_listener(struct listen_t *l, const struct addrinfo *ai)
{
	int arg;
	int fd, i;
	struct client_udp_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */

	hlog(LOG_INFO, "Binding listening UDP socket: %s", l->addr_s);
	
	if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
		hlog(LOG_CRIT, "socket(): %s\n", strerror(errno));
		return -1;
	}
	
	arg = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (char *)&arg, sizeof(arg));
#endif

	memcpy( &sa, ai->ai_addr,  ai->ai_addrlen );
	
	if (bind(fd, ai->ai_addr, ai->ai_addrlen)) {
		hlog(LOG_CRIT, "bind(%s): %s", l->addr_s, strerror(errno));
		close(fd);
		return -1;
	}

	c = client_udp_alloc(fd, l->portnum);
	c->portaccount = l->portaccount;

	inbound_connects_account(3, c->portaccount); /* "3" = udp, not listening.. 
							account all ports + port-specifics */

	if (1) {
		int len, arg;
		/* Set bigger SNDBUF size for the UDP port..  */
		len = sizeof(arg);
		arg = 128*1024;
		/* i = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &arg, len); */
		i = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &arg, len);
	}

	/* set non-blocking mode */
	fcntl(c->fd, F_SETFL, O_NONBLOCK);
	/* it really can't fail.. and socket usage is  sendto(... MSG_DONTWAIT),
	   so it doesn't really even matter if it fails. */

	l->udp = c;
	/* l->fd = fd -- except that close on it will kill working client setups.. */
	
	return fd;
}

static int open_listeners(void)
{
	struct listen_config_t *lc;
	struct listen_t *l;
	int opened = 0, i;
	
	for (lc = listen_config; (lc); lc = lc->next) {
		l = listener_alloc();

		l->clientflags = lc->client_flags;
		l->clients_max = lc->clients_max;

		l->portaccount = port_accounter_alloc();

		/* Pick first of the AIs for this listen definition */

		l->addr_s = strsockaddr( lc->ai->ai_addr, lc->ai->ai_addrlen );
		l->name   = hstrdup(lc->name);
		
		if (lc->ai->ai_socktype == SOCK_DGRAM &&
		    lc->ai->ai_protocol == IPPROTO_UDP) {
			/* UDP listenting is not quite same as TCP listening.. */
			i = open_udp_listener(l, lc->ai);
		} else {
			/* TCP listenting... */
			i = open_tcp_listener(l, lc->ai);
		}

		if (i >= 0) {
			opened++;
			hlog(LOG_DEBUG, "... ok, bound");
		} else {
			hlog(LOG_DEBUG, "... failed");
			listener_free(l);
			continue;
		}
		
		/* Copy access lists */
		if (lc->acl)
			l->acl = acl_dup(lc->acl);

		/* Copy filter definitions */
		for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
			if (i < (sizeof(lc->filters)/sizeof(lc->filters[0])))
				l->filters[i] = (lc->filters[i]) ? hstrdup(lc->filters[i]) : NULL;
			else
				l->filters[i] = NULL;
		}

		hlog(LOG_DEBUG, "... adding %s to listened sockets", l->addr_s);
		// put (first) in the list of listening sockets
		l->next = listen_list;
		l->prevp = &listen_list;
		if (listen_list)
			listen_list->prevp = &l->next;
		listen_list = l;
	}
	
	return opened;
}

static void close_listeners(void)
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

static struct client_t *do_accept(struct listen_t *l)
{
	int fd, i;
	int pe;
	struct client_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len = sizeof(sa);
	char buf[2000];
	static int next_receiving_worker;
	struct worker_t *w;
	struct worker_t *wc;
	static time_t last_EMFILE_report;
	char *s;


	while (l->udp) {
		/* Received data will be discarded, so receiving it  */
		/* TRUNCATED is just fine.  Sender isn't interesting either.  */
		/* Receive as much as there is -- that is, LOOP...  */

		i = recv( l->udp->fd, buf, sizeof(buf), MSG_DONTWAIT|MSG_TRUNC );

		if (i < 0)
			return 0;  /* no more data */
	}
	
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

			/* Too many open files -- rate limit the reporting -- every 10th second or so.. */
			case EMFILE:
				if (last_EMFILE_report + 10 <= tick) {
					last_EMFILE_report = tick;
					hlog(LOG_ERR, "accept() failed: %s (continuing)", strerror(e));
				}
				return NULL;
			/* Errors reporting system internal/external glitches */
			default:
				hlog(LOG_ERR, "accept() failed: %s (continuing)", strerror(e));
				return NULL;
		}
	}
	
	/* convert client address to string */
	s = strsockaddr( &sa.sa, addr_len );
	
	/* TODO: the dropped connections here are not accounted. */
	
	/* limit amount of connections per port... should probably have an error
	 * message to the client (javaprssrvr has one)
	 */
	if (l->portaccount->gauge >= l->clients_max) {
		hlog(LOG_INFO, "%s - Denied client on fd %d from %s: Too many clients (%d)", l->addr_s, fd, s, l->portaccount->gauge);
		close(fd);
		hfree(s);
		inbound_connects_account(-1, c->portaccount); /* account rejected connection */
		return NULL;
	}
	
	/* match against acl... could probably have an error message to the client */
	if (l->acl) {
		if (!acl_check(l->acl, (struct sockaddr *)&sa, addr_len)) {
			hlog(LOG_INFO, "%s - Denied client on fd %d from %s (ACL)", l->addr_s, fd, s);
			close(fd);
			hfree(s);
			inbound_connects_account(-1, c->portaccount); /* account rejected connection */
			return NULL;
		}
	}
	
	c = client_alloc();
	c->fd    = fd;
	c->addr  = sa;
	c->portnum = l->portnum;
	c->state = CSTATE_LOGIN;
	c->flags     = l->clientflags;
	/* use the default login handler */
	c->handler = &login_handler;
	c->udpclient = client_udp_find(l->portnum);
	c->portaccount = l->portaccount;
	c->keepalive = tick;
	c->connect_time = tick;
	c->last_read = tick; /* not simulated time */
	inbound_connects_account(1, c->portaccount); /* account all ports + port-specifics */

	/* text format of client's IP address + port */

#ifndef FIXED_IOBUFS
	c->addr_rem = s;
#else
	strncpy(c->addr_rem, s, sizeof(c->addr_rem));
	c->addr_rem[sizeof(c->addr_rem)-1] = 0;
	hfree(s);
#endif

	/* hex format of client's IP address + port */

	s = hexsockaddr( &sa.sa, addr_len );
#ifndef FIXED_IOBUFS
	c->addr_hex = s;
#else
	strncpy(c->addr_hex, s, sizeof(c->addr_hex));
	c->addr_hex[sizeof(c->addr_hex)-1] = 0;
	hfree(s);
#endif

	/* text format of servers' connected IP address + port */

	addr_len = sizeof(sa);
	if (getsockname(fd, &sa.sa, &addr_len) == 0) { /* Fails very rarely.. */
	  /* present my socket end address as a malloced string... */
	  s = strsockaddr( &sa.sa, addr_len );
	} else {
	  s = hstrdup( l->addr_s ); /* Server's bound IP address */
	}
#ifndef FIXED_IOBUFS
	c->addr_loc = s;
#else
	strncpy(c->addr_loc, s, sizeof(c->addr_loc));
	c->addr_loc[sizeof(c->addr_loc)-1] = 0;
	hfree(s);
#endif

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

	if (c->flags & CLFLAGS_UPLINKSIM)
		uplink_simulator = 1;

	hlog(LOG_DEBUG, "%s - Accepted client on fd %d from %s", c->addr_loc, c->fd, c->addr_rem);
	
	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
		if (l->filters[i])
			if (filter_parse(c, l->filters[i], 0) < 0) { /* system filters */
				hlog(LOG_ERR, "Bad system filter definition: %s", l->filters[i]);
			}
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
	
	int client_min = -1;
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

	inbound_connects_account(0, c->portaccount); /* something failed, remove this from accounts.. */
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
				int fd = l->udp ? l->udp->fd : l->fd;
				hlog(LOG_DEBUG, "... %d: fd %d (%s)", n, fd, l->addr_s);
				acceptpfd[n].fd = fd;
				acceptpfd[n].events = POLLIN|POLLPRI|POLLERR|POLLHUP;
				n++;
			}
			hlog(LOG_INFO, "Accept thread ready.");
			
			/* stop the dupechecking and uplink threads while adjusting
			 * the amount of workers... they walk the worker list, and
			 * might get confused when workers are stopped or started.
			 */
			if (workers_running != workers_configured) {
				uplink_stop();
				dupecheck_stop();
				workers_start();
				dupecheck_start();
				uplink_start();
			}
		}
		
		/* check for new connections */
		e = poll(acceptpfd, listen_n, 200);
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
			if (!(l) || (l->udp ? l->udp->fd : l->fd) != acceptpfd[n].fd) {
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

int accept_listener_status(cJSON *listeners)
{
	int n = 0;
	struct listen_t *l;
	
	for (l = listen_list; (l); l = l->next) {
		cJSON *jl = cJSON_CreateObject();
		cJSON_AddNumberToObject(jl, "fd", l->fd);
		cJSON_AddStringToObject(jl, "name", l->name);
		cJSON_AddStringToObject(jl, "addr", l->addr_s);
		cJSON_AddNumberToObject(jl, "clients", l->portaccount->gauge);
		cJSON_AddNumberToObject(jl, "clients_peak", l->portaccount->gauge_max);
		cJSON_AddNumberToObject(jl, "clients_max", l->clients_max);
		cJSON_AddNumberToObject(jl, "connects", l->portaccount->counter);
		cJSON_AddNumberToObject(jl, "bytes_rx", l->portaccount->rxbytes);
		cJSON_AddNumberToObject(jl, "bytes_tx", l->portaccount->txbytes);
		cJSON_AddNumberToObject(jl, "pkts_rx", l->portaccount->rxpackets);
		cJSON_AddNumberToObject(jl, "pkts_tx", l->portaccount->txpackets);
		cJSON_AddItemToArray(listeners, jl);
	}
	
	return n;
}
