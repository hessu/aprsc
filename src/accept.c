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
#include <netinet/tcp.h>
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
#include "http.h"
#include "incoming.h" /* incoming_handler prototype */
#include "uplink.h"

struct listen_t {
	struct listen_t *next;
	struct listen_t **prevp;
	
	int fd;
	int clientflags;
	int portnum;
	int clients_max;
	int corepeer;
	int hidden;
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

/* pseudoworker + pseudoclient for incoming UDP packets */
struct worker_t *udp_worker = NULL;
struct client_t *udp_pseudoclient = NULL;


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
 *	Pass a new client to a worker thread
 */

static int accept_pass_client_to_worker(struct worker_t *wc, struct client_t *c)
{
	int pe;
	
	hlog(LOG_DEBUG, "... passing client to thread %d with %d users", wc->id, wc->client_count);

	if ((pe = pthread_mutex_lock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "do_accept(): could not lock new_clients_mutex: %s", strerror(pe));
		return -1;
	}
	
	/* push the client in the worker's queue */
	c->next = NULL;
	
	if (wc->new_clients_last) {
		wc->new_clients_last->next = c;
		c->prevp = &wc->new_clients_last->next;
	} else {
		wc->new_clients = c;
		c->prevp = &wc->new_clients;
	}
	
	wc->new_clients_last = c;
	
	/* unlock the queue */
	if ((pe = pthread_mutex_unlock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "do_accept(): could not unlock new_clients_mutex: %s", strerror(pe));
		return -1;
	}
	
	return 0;
}

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
	int fd;
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

	c = client_udp_alloc((l->corepeer) ? &udppeers : &udpclients, fd, l->portnum);
	c->af = ai->ai_family;
	c->portaccount = l->portaccount;

	inbound_connects_account(3, c->portaccount); /* "3" = udp, not listening.. 
							account all ports + port-specifics */

	if (1) {
		int len, arg;
		/* Set bigger SNDBUF size for the UDP port..  */
		len = sizeof(arg);
		arg = 12*1024; /* 20 Kbytes is good for about 5 seconds of APRS-IS full feed */
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &arg, len)) {
			hlog(LOG_ERR, "UDP listener setup: setsockopt(SO_RCVBUF, %d) failed: %s", arg, strerror(errno));
		}
		if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &arg, len)) {
			hlog(LOG_ERR, "UDP listener setup: setsockopt(SO_SNDBUF, %d) failed: %s", arg, strerror(errno));
		}
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
		l->hidden = lc->hidden;
		l->corepeer = lc->corepeer;
		l->clientflags = lc->client_flags;
		l->clients_max = lc->clients_max;

		l->portaccount = port_accounter_alloc();

		/* Pick first of the AIs for this listen definition */

		l->addr_s = strsockaddr( lc->ai->ai_addr, lc->ai->ai_addrlen );
		l->name   = hstrdup(lc->name);
		l->portnum = lc->portnum;
		
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
 *	Generate UDP peer "clients"
 */

static void peerip_clients_config(void)
{
	struct client_t *c;
	struct peerip_config_t *pe;
	struct client_udp_t *udpclient;
	char *s;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len = sizeof(sa);
	
	for (pe = peerip_config; (pe); pe = pe->next) {
		hlog(LOG_DEBUG, "Setting up UDP peer %s (%s)", pe->name, pe->host);
		udpclient = client_udp_find(udppeers, pe->af, pe->local_port);
		
		if (!udpclient) {
			hlog(LOG_ERR, "Failed to find UDP socket on port %d for peer %s (%s)", pe->local_port, pe->name, pe->host);
			continue;
		}

		c = client_alloc();
		if (!c) {
			hlog(LOG_ERR, "peerip_clients_config: client_alloc returned NULL");
			abort();
		}
		c->fd = -1; // Right, this client will never have a socket of it's own.
		c->portnum = pe->local_port; // local port
		c->state = CSTATE_COREPEER;
		c->validated = 1;
		c->flags = CLFLAGS_UPLINKPORT;
		c->handler = &incoming_handler;
		memcpy((void *)&c->udpaddr.sa, (void *)pe->ai->ai_addr, pe->ai->ai_addrlen);
		c->udpaddrlen = pe->ai->ai_addrlen;
		c->udp_port = pe->remote_port; // remote port
		c->addr = c->udpaddr;
		c->udpclient = udpclient;
		//c->portaccount = l->portaccount;
		c->keepalive = tick;
		c->connect_time = tick;
		c->last_read = tick; /* not simulated time */
		
		/* convert client address to string */
		s = strsockaddr( &c->udpaddr.sa, c->udpaddrlen );
		
		/* text format of client's IP address + port */
#ifndef FIXED_IOBUFS
		c->addr_rem = s;
#else
		strncpy(c->addr_rem, s, sizeof(c->addr_rem));
		c->addr_rem[sizeof(c->addr_rem)-1] = 0;
		hfree(s);
#endif
		
		/* hex format of client's IP address + port */
		s = hexsockaddr( &c->udpaddr.sa, c->udpaddrlen );
		
#ifndef FIXED_IOBUFS
		c->addr_hex = s;
#else
		strncpy(c->addr_hex, s, sizeof(c->addr_hex));
		c->addr_hex[sizeof(c->addr_hex)-1] = 0;
		hfree(s);
#endif

		/* text format of servers' connected IP address + port */
		addr_len = sizeof(sa);
		if (getsockname(c->udpclient->fd, &sa.sa, &addr_len) == 0) { /* Fails very rarely.. */
			/* present my socket end address as a malloced string... */
			s = strsockaddr( &sa.sa, addr_len );
		} else {
			s = hstrdup( "um" ); /* Server's bound IP address.. TODO: what? */
		}
#ifndef FIXED_IOBUFS
		c->addr_loc = s;
#else
		strncpy(c->addr_loc, s, sizeof(c->addr_loc));
		c->addr_loc[sizeof(c->addr_loc)-1] = 0;
		hfree(s);
#endif

		/* pass the client to the first worker thread */
		accept_pass_client_to_worker(worker_threads, c);
	}
}

/*
 *	Close and free UDP peer "clients"
 */

static void peerip_clients_close(void)
{
	struct client_t *c;
	
	c = client_alloc();
	if (!c) {
		hlog(LOG_ERR, "peerip_clients_close: client_alloc returned NULL");
		abort();
	}
	
	c->fd = -2; // Magic FD to close them all
	c->state = CSTATE_COREPEER;
	sprintf(c->filter_s, "peerip_clients_close"); // debugging
	accept_pass_client_to_worker(worker_threads, c);
}

/*
 *	Process an incoming UDP packet submission
 */

static void accept_process_udpsubmit(struct listen_t *l, char *buf, int len, char *remote_host)
{
	int packet_len;
	char *login_string = NULL;
	char *packet = NULL;
	char *username = NULL;
	char validated;
	int e;
	
	//hlog(LOG_DEBUG, "got udp submit: %.*s", len, buf);
	
	packet_len = loginpost_split(buf, len, &login_string, &packet);
	if (packet_len == -1) {
		hlog(LOG_DEBUG, "UDP submit [%s]: No newline (LF) found in data", remote_host);
		return;
	}
	
	if (!login_string) {
		hlog(LOG_DEBUG, "UDP submit [%s]: No login string in data", remote_host);
		return;
	}
	
	if (!packet) {
		hlog(LOG_DEBUG, "UDP submit [%s]: No packet data found in data", remote_host);
		return;
	}
	
	hlog(LOG_DEBUG, "UDP submit [%s]: login string: %s", remote_host, login_string);
	hlog(LOG_DEBUG, "UDP submit [%s]: packet: %s", remote_host, packet);
	
	/* process the login string */
	validated = http_udp_upload_login(remote_host, login_string, &username);
	if (validated < 0) {
		hlog(LOG_DEBUG, "UDP submit [%s]: Invalid login string", remote_host);
		return;
	}
	
	if (validated != 1) {
		hlog(LOG_DEBUG, "UDP submit [%s]: Invalid passcode for user %s", remote_host, username);
		return;
	}
	
	/* packet size limits */
	if (packet_len < PACKETLEN_MIN) {
		hlog(LOG_DEBUG, "UDP submit [%s]: Packet too short: %d bytes", remote_host, packet_len);
		return;
	}
	
	if (packet_len > PACKETLEN_MAX-2) {
		hlog(LOG_DEBUG, "UDP submit [%s]: Packet too long: %d bytes", remote_host, packet_len);
		return;
	}
	
	e = pseudoclient_push_packet(udp_worker, udp_pseudoclient, username, packet, packet_len);

	if (e < 0)
		hlog(LOG_DEBUG, "UDP submit [%s]: Incoming packet parse failure code %d: %s", remote_host, e, packet);
	else
		hlog(LOG_DEBUG, "UDP submit [%s]: Incoming packet parsed, code %d: %s", remote_host, e, packet);
}

/*
 *	Receive UDP packets from an UDP listener
 */

static void accept_udp_recv(struct listen_t *l)
{
	union sockaddr_u addr;
	socklen_t addrlen;
	char buf[2000];
	int i;
	char *addrs;
	
	/* Receive as much as there is -- that is, LOOP...  */
	addrlen = sizeof(addr);
	
	while ((i = recvfrom( l->udp->fd, buf, sizeof(buf)-1, MSG_DONTWAIT|MSG_TRUNC, (struct sockaddr *)&addr, &addrlen )) >= 0) {
		if (!(l->clientflags & CLFLAGS_UDPSUBMIT)) {
			hlog(LOG_DEBUG, "accept thread discarded an UDP packet on a listening socket");
			continue;
		}
		
		addrs = strsockaddr(&addr.sa, addrlen);
		accept_process_udpsubmit(l, buf, i, addrs);
		hfree(addrs);
	}
}

/*
 *	Accept a single connection
 */

static void do_accept(struct listen_t *l)
{
	int fd, i;
	struct client_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len = sizeof(sa);
	static int next_receiving_worker;
	struct worker_t *w;
	struct worker_t *wc;
	static time_t last_EMFILE_report;
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

			/* Too many open files -- rate limit the reporting -- every 10th second or so.. */
			case EMFILE:
				if (last_EMFILE_report + 10 <= tick) {
					last_EMFILE_report = tick;
					hlog(LOG_ERR, "accept() failed: %s (continuing)", strerror(e));
				}
				return;
			/* Errors reporting system internal/external glitches */
			default:
				hlog(LOG_ERR, "accept() failed: %s (continuing)", strerror(e));
				return;
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
		inbound_connects_account(-1, l->portaccount); /* account rejected connection */
		return;
	}
	
	/* match against acl... could probably have an error message to the client */
	if (l->acl) {
		if (!acl_check(l->acl, (struct sockaddr *)&sa, addr_len)) {
			hlog(LOG_INFO, "%s - Denied client on fd %d from %s (ACL)", l->addr_s, fd, s);
			close(fd);
			hfree(s);
			inbound_connects_account(-1, l->portaccount); /* account rejected connection */
			return;
		}
	}
	
	c = client_alloc();
	
	if (!c) {
		hlog(LOG_ERR, "%s - client_alloc returned NULL, too many clients. Denied client on fd %d from %s (ACL)", l->addr_s, fd, s);
		close(fd);
		hfree(s);
		inbound_connects_account(-1, l->portaccount); /* account rejected connection */
	}
	
	c->fd    = fd;
	c->addr  = sa;
	c->portnum = l->portnum;
	c->hidden  = l->hidden;
	c->state   = CSTATE_LOGIN;
	c->flags   = l->clientflags;
	/* use the default login handler */
	c->handler = &login_handler;
	c->udpclient = client_udp_find(udpclients, sa.sa.sa_family, l->portnum);
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
	
	/* Use TCP_NODELAY for APRS-IS sockets. High delays can cause packets getting past
	 * the dupe filters.
	 */
#ifdef TCP_NODELAY
	int arg = 1;
	if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (void *)&arg, sizeof(arg)))
		hlog(LOG_ERR, "%s - Accept: setsockopt(TCP_NODELAY, %d) failed: %s", l->addr_s, arg, strerror(errno));
#endif

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

	/* ok, found it... lock the new client queue and pass the client */
	if (accept_pass_client_to_worker(wc, c))
		goto err;
	
	return;
	
err:

	inbound_connects_account(0, c->portaccount); /* something failed, remove this from accounts.. */
	client_free(c);
	return;
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
	
	/* we allocate a worker structure to be used within the accept thread
	 * for parsing incoming UDP packets and passing them on to the dupecheck
	 * thread.
	 */
	udp_worker = worker_alloc();
	udp_worker->id = 81;
	
	/* we also need a client structure to be used with incoming
	 * HTTP position uploads
	 */
	udp_pseudoclient = pseudoclient_setup(81);
	udp_pseudoclient->flags |= CLFLAGS_UDPSUBMIT;
	
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
				/* The accept thread does not poll() UDP sockets for core peers.
				 * Worker 0 takes care of that, and processes the incoming packets.
				 */
				if (l->corepeer) {
					hlog(LOG_DEBUG, "... %d: fd %d (%s) - not polled, is corepeer", n, (l->udp) ? l->udp->fd : l->fd, l->addr_s);
					listen_n--;
					continue;
				}
				
				int fd;
				if (l->udp) {
					l->udp->polled = 1;
					fd = l->udp->fd;
				} else {
					fd = l->fd;
				}
				
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
			} else {
				/* if uplink was not restarted, reconfigure it */
				uplink_reconfiguring = 1;
			}
			
			/*
			 * generate UDP peer clients
			 */
			peerip_clients_close();
			if (peerip_config)
				peerip_clients_config();
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
			if (acceptpfd[n].revents) {
				if (l->udp)
					accept_udp_recv(l); /* receive UDP packets */
				else
					do_accept(l); /* accept a single connection */
			}
			l = l->next;
		}
	}
	
	hlog(LOG_DEBUG, "Accept thread shutting down listening sockets and worker threads...");
	uplink_stop();
	close_listeners();
	dupecheck_stop();
	http_shutting_down = 1;
	workers_stop(1);
	hfree(acceptpfd);
	
	/* free up the pseudo-client */
	client_free(udp_pseudoclient);
	udp_pseudoclient = NULL;
	
	/* free up the pseudo-worker structure, after dupecheck is long dead */
	worker_free_buffers(udp_worker);
	hfree(udp_worker);
	udp_worker = NULL;
}

int accept_listener_status(cJSON *listeners, cJSON *totals)
{
	int n = 0;
	struct listen_t *l;
	long total_clients = 0;
	long total_connects = 0;
	/*
	 * These aren't totals, these are only for clients, not uplinks.
	 * So, disregard for now.
	long long total_rxbytes = 0;
	long long total_txbytes = 0;
	long long total_rxpackets = 0;
	long long total_txpackets = 0;
	*/
	
	for (l = listen_list; (l); l = l->next) {
		if (l->corepeer || l->hidden)
			continue;
		cJSON *jl = cJSON_CreateObject();
		cJSON_AddNumberToObject(jl, "fd", l->fd);
		cJSON_AddStringToObject(jl, "name", l->name);
		cJSON_AddStringToObject(jl, "proto", (l->udp) ? "udp" : "tcp");
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
		
		total_clients += l->portaccount->gauge;
		total_connects += l->portaccount->counter;
		/*
		total_rxbytes += l->portaccount->rxbytes;
		total_txbytes += l->portaccount->txbytes;
		total_rxpackets += l->portaccount->rxpackets;
		total_txpackets += l->portaccount->txpackets;
		*/
	}
	
	cJSON_AddNumberToObject(totals, "clients", total_clients);
	cJSON_AddNumberToObject(totals, "connects", total_connects);
	/*
	cJSON_AddNumberToObject(totals, "bytes_rx", total_rxbytes);
	cJSON_AddNumberToObject(totals, "bytes_tx", total_txbytes);
	cJSON_AddNumberToObject(totals, "pkts_rx", total_rxpackets);
	cJSON_AddNumberToObject(totals, "pkts_tx", total_txpackets);
	*/
	
	return n;
}
