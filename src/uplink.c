/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

/*
 *	uplink.c: processes uplink data within the worker thread
 */

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <alloca.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>

#include "config.h"
#include "uplink.h"
#include "hmalloc.h"
#include "hlog.h"
#include "worker.h"
#include "login.h"
#include "incoming.h"
#include "outgoing.h"
#include "filter.h"
#include "passcode.h"

#define MAX_UPLINKS 32

int uplink_reconfiguring;
int uplink_shutting_down;

pthread_mutex_t uplink_client_mutex = PTHREAD_MUTEX_INITIALIZER;
struct client_t *uplink_client[MAX_UPLINKS];

int uplink_running;
pthread_t uplink_th;

/* global uplink connects, and protocol traffic accounters */

struct portaccount_t uplink_connects = {
  .mutex    = PTHREAD_MUTEX_INITIALIZER,
  .refcount = 99,	/* Global static blocks have extra-high initial refcount */
};


/*
 *	signal handler
 */
 
int uplink_sighandler(int signum)
{
	switch (signum) {
		
	default:
		hlog(LOG_WARNING, "* SIG %d ignored", signum);
		break;
	}
	
	signal(signum, (void *)uplink_sighandler);	/* restore handler */
	return 0;
}

/*
 *	Close uplinking sockets
 */

void close_uplinkers(void)
{
	int rc;
	
	hlog(LOG_INFO, "Closing all uplinks");

	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog( LOG_ERR, "close_uplinkers(): could not lock uplink_client_mutex: %s", strerror(rc) );
		return;
	}
	
	int i;
	for (i = 0; i < MAX_UPLINKS; i++) {
		if ((uplink_client[i]) && uplink_client[i]->fd >= 0) {
			hlog( LOG_DEBUG, "Closing uplinking socket %d (fd %d) %s ...", i, uplink_client[i]->fd, uplink_client[i]->addr_rem );
			shutdown(uplink_client[i]->fd, SHUT_RDWR);
		}
	}
	
	if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog( LOG_ERR, "close_uplinkers(): could not unlock uplink_client_mutex: %s", strerror(rc) );
		return;
	}
	return;
}

void uplink_close(struct client_t *c, int errnum)
{
	int rc;

	if (errnum == 0)
		hlog(LOG_INFO, "%s: Uplink has been closed.", c->addr_rem);
	else if (errnum == -1)
		hlog(LOG_INFO, "%s: Uplink has been closed by remote host (EOF).", c->addr_rem);
	else if (errnum == -2)
		hlog(LOG_INFO, "%s: Uplink has been closed due to timeout.", c->addr_rem);
	else
		hlog(LOG_INFO, "%s: Uplink has been closed due to error: %s", c->addr_rem, strerror(errnum));

	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "close_uplinkers(): could not lock uplink_client_mutex: %s", strerror(rc));
		return;
	}

	-- uplink_connects.gauge;
	
	struct uplink_config_t *l = uplink_config;
	for (; l; l = l->next) {
		if (l->client_ptr == (void *)c) {
			hlog(LOG_DEBUG, "found the link to disconnect");
			l->state = UPLINK_ST_NOT_LINKED;
			l->client_ptr = NULL;
		}
	}
	
	uplink_client[c->uplink_index] = NULL; // there can be only one!

	if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "close_uplinkers(): could not unlock uplink_client_mutex: %s", strerror(rc));
		return;
	}
	return;
}


int uplink_login_handler(struct worker_t *self, struct client_t *c, char *s, int len)
{
	char buf[1000];
	int passcode, rc;
	int argc;
	char *argv[256];

#ifndef FIXED_IOBUFS
	if (!c->username)
		c->username = hstrdup("simulator");
#else
	if (!*c->username)
		strcpy(c->username, "simulator");
#endif

	passcode = aprs_passcode(c->username);

	hlog(LOG_INFO, "%s: Uplink server says: \"%.*s\"", c->addr_rem, len, s);
	
	/* parse to arguments */
	/* make it null-terminated for our string processing */
	char *e = s + len;
	*e = 0;
	if ((argc = parse_args_noshell(argv, s)) == 0 || *argv[0] != '#') {
		hlog(LOG_ERR, "%s: Uplink's welcome message is not recognized", c->addr_rem);
		return 0;
	}
	
	if (argc >= 3) {
#ifndef FIXED_IOBUFS
		c->app_name = hstrdup(argv[1]);
		c->app_version = hstrdup(argv[2]);
#else
		strncpy(c->app_name, argv[1], sizeof(c->app_name));
		c->app_name[sizeof(c->app_name)-1] = 0;
		strncpy(c->app_version, argv[2], sizeof(c->app_version));
		c->app_version[sizeof(c->app_version)-1] = 0;
#endif
	}

	// FIXME: Send the login string... (filters missing ???)

	len = sprintf(buf, "user %s pass %d vers %s\r\n", c->username, passcode, VERSTR);

	hlog(LOG_DEBUG, "%s: my login string: \"%.*s\"", c->addr_rem, len-2, buf, len);

	rc = client_write(self, c, buf, len);
	if (rc < -2) return rc;

	c->handler = incoming_handler;
	c->state   = CSTATE_CONNECTED;
	
	hlog(LOG_INFO, "%s: Connected to server, logging in", c->addr_rem);
	
	return 0;
}


/*
 *	Uplink a single connection
 */

int make_uplink(struct uplink_config_t *l)
{
	int fd, i, arg;
	int uplink_index;
	struct client_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len;
	struct addrinfo *ai, *a, *ap[21];
	struct addrinfo req;
	char addr_s[180];
	char *s;
	int port;
	int pe;
	struct worker_t *wc;

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;
	
	/* find a free uplink slot */
	for (uplink_index = 0; uplink_index < MAX_UPLINKS; uplink_index++) {
		if (!uplink_client[uplink_index])
			break;
	}
	if (uplink_index == MAX_UPLINKS) {
		hlog(LOG_ERR, "Uplink: No available uplink slots, %d used", MAX_UPLINKS);
		return -2;
	}
	
	if (strcasecmp(l->proto, "tcp") == 0) {
		// well, do nothing for now.
	} else if (strcasecmp(l->proto, "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#if defined(SOCK_SEQPACKET) && defined(IPPROTO_SCTP)
	} else if (strcasecmp(l->proto, "sctp") == 0) {
		req.ai_socktype = SOCK_SEQPACKET;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "Uplink: Unsupported protocol '%s'\n", l->proto);
		return -2;
	}
	
	port = atoi(l->port);
	if (port < 1 || port > 65535) {
		hlog(LOG_ERR, "Uplink: unsupported port number '%s'\n", l->port);
		return -2;
	}

	l->state = UPLINK_ST_CONNECTING;
	i = getaddrinfo(l->host, l->port, &req, &ai);
	if (i != 0) {
		hlog(LOG_INFO,"Uplink: address resolving failure of '%s' '%s'", l->host, l->port);
		l->state = UPLINK_ST_NOT_LINKED;
		return i;
	}

	i = 0;
	for (a = ai; a && i < 20 ; a = a->ai_next, ++i) {
		ap[i] = a; /* Up to 20 first addresses */
	}
	ap[i] = NULL;
	/* If more than one, pick one at random, and place it as list leader */
	if (i > 0)
		i = random() % i;

	if (i > 0) {
		a = ap[i];
		ap[i] = ap[0];
		ap[0] = a;
	}
	i = 0;

	/* Then lets try making socket and connection in address order */
	fd = -1;
	while (( a = ap[i++] )) {

		// FIXME: format socket IP address to text
		sprintf(addr_s, "%s:%s", l->host, l->port);

		hlog(LOG_INFO, "Uplink: Connecting to %s:%s", l->host, l->port);
	
		if ((fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) < 0) {
			hlog(LOG_CRIT, "Uplink: socket(): %s\n", strerror(errno));
			continue;
		}

		arg = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg));
		
		/* set non-blocking mode at this point, so that we can make a
		 * non-blocking connect() with a short timeout
		 */
		if (fcntl(fd, F_SETFL, O_NONBLOCK)) {
			hlog(LOG_CRIT, "Uplink: Failed to set non-blocking mode on new socket: %s", strerror(errno));
			close(fd);
			fd = -1;
			continue;
		}
		
		if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) {
			hlog(LOG_ERR, "Uplink: connect(%s) failed: %s", addr_s, strerror(errno));
			close(fd);
			fd = -1;
			continue;
		}
		
		/* Only wait a few seconds for the connection to be created.
		 * If the connection setup is very slow, it is unlikely to
		 * perform well enough anyway.
		 */
		struct pollfd connect_fd;
		connect_fd.fd = fd;
		connect_fd.events = POLLOUT;
		connect_fd.revents = 0;
		
		int r = poll(&connect_fd, 1, 3000);
		hlog(LOG_DEBUG, "Uplink: poll after connect returned %d, revents %d", r, connect_fd.revents);
		
		if (r < 0) {
			hlog(LOG_ERR, "Uplink: connect to %s: poll failed: %s", addr_s, strerror(errno));
			close(fd);
			fd = -1;
			continue;
		}
		
		if (r < 1) {
			hlog(LOG_ERR, "Uplink: connect to %s timed out", addr_s);
			close(fd);
			fd = -1;
			continue;
		}
		
		socklen_t optlen = sizeof(arg);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&arg, &optlen);
		if (arg == 0) {
			/* Successful connect! */
			hlog(LOG_DEBUG, "Uplink: successfull connect");
			break;
		}
		
		hlog(LOG_ERR, "Uplink: connect to %s failed: %s", addr_s, strerror(arg));
		close(fd);
		fd = -1;
	}

	freeaddrinfo(ai); /* Not needed anymore.. */

	if (fd < 0) {
		l->state = UPLINK_ST_NOT_LINKED;
		return -3; /* No successfull connection at any address.. */
	}

	c = client_alloc();
	l->client_ptr = (void *)c;
	c->uplink_index = uplink_index;
	c->fd    = fd;
	c->addr  = sa;
	c->state = CSTATE_CONNECTED;
	/* use the default login handler */
	c->handler  = & uplink_login_handler;
	c->flags    = l->client_flags;
	c->keepalive = tick;
	c->connect_time = tick;
	c->last_read = tick; /* not simulated time */
#ifndef FIXED_IOBUFS
	c->username = hstrdup(mycall);
#else
	strncpy(c->username, mycall, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
#endif


	/* These peer/sock name calls can not fail -- or the socket closed
	   on us in which case it gets abandoned a bit further below. */

	addr_len = sizeof(sa);
	getpeername(fd, (struct sockaddr *)&sa, &addr_len);
	s = strsockaddr( &sa.sa, addr_len ); /* server side address */
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

	addr_len = sizeof(sa);
	getsockname(fd, (struct sockaddr *)&sa, &addr_len);
	s = strsockaddr( &sa.sa, addr_len ); /* client side address */
#ifndef FIXED_IOBUFS
	c->addr_loc = s;
#else
	strncpy(c->addr_loc, s, sizeof(c->addr_loc));
	c->addr_loc[sizeof(c->addr_loc)-1] = 0;
	hfree(s);
#endif

	hlog(LOG_INFO, "%s: %s: Uplink connection established fd %d using source address %s", c->addr_rem, l->name, c->fd, c->addr_loc);

	uplink_client[uplink_index] = c;
	l->state = UPLINK_ST_CONNECTED;
	
	// for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
	// 	if (l->filters[i])
	// 		filter_parse(c, l->filters[i], 0); /* system filters */
	// }

	/* Push it on the first worker, which ever it is..
	 */

	wc = worker_threads;
	
	hlog(LOG_DEBUG, "... passing to worker thread %d with %d users", wc->id, wc->client_count);
	if ((pe = pthread_mutex_lock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "make_uplink(): could not lock new_clients_mutex: %s", strerror(pe));
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
		hlog(LOG_ERR, "make_uplink(): could not unlock new_clients_mutex: %s", strerror(pe));
		goto err;
	}
	
	++ uplink_connects.gauge;
	++ uplink_connects.counter;
	++ uplink_connects.refcount;  /* <-- that does not get decremented at any time..  */
	
	c->portaccount = & uplink_connects; /* calculate traffic bytes/packets */
	
	return 0;
	
err:
	client_free(c);
	uplink_client[uplink_index] = NULL;
	l->state = UPLINK_ST_NOT_LINKED;
	return -1;
}


/*
 *	Uplink thread
 */

void uplink_thread(void *asdf)
{
	sigset_t sigs_to_block;
	int rc;
	int next_uplink = -1; /* the index to the next regular uplink candidate */
	
	pthreads_profiling_reset("uplink");
	
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
	
	hlog(LOG_INFO, "Uplink thread starting...");
	
	uplink_reconfiguring = 1;
	while (!uplink_shutting_down) {
		if (uplink_reconfiguring) {
			uplink_reconfiguring = 0;
			close_uplinkers();

			hlog(LOG_INFO, "Uplink thread ready.");
		}
		
		/* sleep for 1 second */
		poll(NULL, 0, 1000);
		
		/* speed up shutdown */
		if (uplink_shutting_down)
			continue;
		
		if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
			hlog(LOG_ERR, "uplink_thread(): could not lock uplink_client_mutex: %s", strerror(rc));
			continue;
		}
		
		/* Check if all we have a single regular uplink connection up, out of all
		 * the configured ones. Also, check that all the UPLINKMULTI links are
		 * connected.
		 */
		
		int has_uplink = 0; /* do we have a single regular uplink? */
		int avail_uplink = 0; /* how many regular uplinks are configured? */
		
		struct uplink_config_t *l = uplink_config;
		for (; l; l = l->next) {
			if (l->client_flags & CLFLAGS_UPLINKMULTI) {
				/* MULTI uplink, needs to be up */
				if (l->state < UPLINK_ST_CONNECTING)
					make_uplink(l);
			} else {
				/* regular uplink, need to have one connected */
				if (l->state >= UPLINK_ST_CONNECTING)
					has_uplink++;
				avail_uplink++;
			}
		}
		
		if (avail_uplink && !has_uplink) {
			hlog(LOG_INFO, "Uplink: %d uplinks configured, %d are connected, need to pick new", avail_uplink, has_uplink);
			/* we have regular uplinks but none are connected,
			 * pick the next one and connect */
			next_uplink++;
			if (next_uplink >= avail_uplink)
				next_uplink = 0;
			hlog(LOG_DEBUG, "Uplink: picked uplink index %d as the new candidate", next_uplink);
			l = uplink_config;
			int i = 0;
			while ((l) && i < next_uplink) {
				if (!(l->client_flags & CLFLAGS_UPLINKMULTI))
					i++;
				l = l->next;
			}
			if (l) {
				hlog(LOG_DEBUG, "Uplink: trying %s (%s:%s)", l->name, l->host, l->port);
				make_uplink(l);
			}
		}
		
		if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
			hlog(LOG_CRIT, "close_uplinkers(): could not unlock uplink_client_mutex: %s", strerror(rc));
			continue;
		}
		
		/* sleep for 4 seconds between successful rounds */
		poll(NULL, 0, 4000);
	}
	
	hlog(LOG_DEBUG, "Uplink thread shutting down uplinking sockets...");
	close_uplinkers();
}


/*
 *	Start / stop the uplinks maintainer thread
 */
 
void uplink_start(void)
{
	if (uplink_running)
		return;
	
	uplink_shutting_down = 0;
	
	if (pthread_create(&uplink_th, &pthr_attrs, (void *)uplink_thread, NULL))
		perror("pthread_create failed for uplink_thread");
		
	uplink_running = 1;
}

void uplink_stop(void)
{
	int e;
	
	if (!uplink_running)
		return;
	
	hlog(LOG_INFO, "Signalling uplink_thread to shut down...");
	uplink_shutting_down = 1;
	
	if ((e = pthread_join(uplink_th, NULL))) { 
		hlog(LOG_ERR, "Could not pthread_join uplink_th: %s", strerror(e));
	} else {
		hlog(LOG_INFO, "Uplink thread has terminated.");
		uplink_running = 0;
	}
}
