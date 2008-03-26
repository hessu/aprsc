/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
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


int uplink_reconfiguring;
int uplink_shutting_down;

pthread_mutex_t uplink_client_mutex = PTHREAD_MUTEX_INITIALIZER;
struct client_t *uplink_client;

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
 *	Open the uplinking socket
 */

void close_uplinkers(void)
{
	int rc;

	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "close_uplinkers(): could not lock uplink_client_mutex: %s", strerror(rc));
		return;
	}

	if (uplink_client && uplink_client->fd >= 0) {
		hlog(LOG_DEBUG, "Closing uplinking socket....");
		shutdown(uplink_client->fd, SHUT_RDWR);
	}

	if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "close_uplinkers(): could not unlock uplink_client_mutex: %s", strerror(rc));
		return;
	}
	return;
}

void uplink_close(struct client_t *c)
{
	int rc;

	hlog(LOG_DEBUG, "Uplink socket has been closed.");

	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "close_uplinkers(): could not lock uplink_client_mutex: %s", strerror(rc));
		return;
	}

	-- uplink_connects.gauge;

	uplink_client = NULL; // there can be only one!

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


	if (!c->username) c->username = hstrdup("simulator");

	passcode = aprs_passcode(c->username);

	hlog(LOG_DEBUG, "%s: server string: '%.*s'", c->addr_s, len, s);

	// FIXME: Send the login string... (filters ???)

	len = sprintf(buf, "user %s pass %d vers %s\r\n", c->username, passcode, VERSTR);

	hlog(LOG_DEBUG, "%s: my login string: '%.*s'", c->addr_s, len-2, buf, len);

	rc = client_write(self, c, buf, len);
	if (rc < -2) return rc;

	c->handler = incoming_handler;
	c->state   = CSTATE_CONNECTED;
	
	return 0;
}


/*
 *	Uplink a single connection
 */

int make_uplink(struct uplink_config_t *l)
{
	int fd, i, arg;
	struct client_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len;
	struct addrinfo *ai, *a;
	struct addrinfo req;
	char addr_s[180];
	int port;
	int pe;
	struct worker_t *wc;

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;

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

	i = getaddrinfo(l->host, l->port, &req, &ai);
	if (i != 0) {
		hlog(LOG_INFO,"Uplink: address resolving failure of '%s' '%s'",l->host,l->port);
		return i;
	}


	i = 0;
	for (a = ai; a ; a = a->ai_next)
	  ++i;
	if (i > 0)
	  i = random() % i;
	else
	  i = -1;

	for (a = ai; a && i > 0; a = a->ai_next, --i)
	  ;
	if (!a) a = ai;

	// FIXME: format socket IP address to text
	sprintf(addr_s, "%s:%s", l->host, l->port);


	hlog(LOG_INFO, "Making uplink TCP socket: %s  %s", l->host, l->port);
	
	if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
		hlog(LOG_CRIT, "socket(): %s\n", strerror(errno));
		return -3;
	}

	arg = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg));
	
	if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
		hlog(LOG_CRIT, "connect(%s): %s", addr_s, strerror(errno));
		close(fd);
		return -3;
	}

	freeaddrinfo(ai); /* Not needed anymore.. */

	addr_len = sizeof(sa);
	if (getpeername(fd, (struct sockaddr *)&sa, &addr_len) != 0) {
		hlog(LOG_CRIT, "getpeername(%s): %s", addr_s, strerror(errno));
		close(fd);
		return -3;
	}

	c = client_alloc();
	c->fd    = fd;
	c->addr  = sa;
	c->state = CSTATE_CONNECTED;
	c->addr_s = strsockaddr( &sa.sa, addr_len );
	c->keepalive = now;
	/* use the default login handler */
	c->handler  = & uplink_login_handler;
	c->username = hstrdup(mycall);
	c->flags    = l->client_flags;

	hlog(LOG_INFO, "%s - Uplink connection on fd %d from %s", addr_s, c->fd, c->addr_s);

	uplink_client = c;


	// for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
	// 	if (l->filters[i])
	// 		filter_parse(c, l->filters[i], 0); /* system filters */
	// }

	/* set non-blocking mode */
	if (fcntl(c->fd, F_SETFL, O_NONBLOCK)) {
		hlog(LOG_ERR, "Uplink: %s - Failed to set non-blocking mode on socket: %s", addr_s, strerror(errno));
		goto err;
	}
	
	/* Push it on the first worker, which ever it is..
	 */

	wc = worker_threads;
	
	hlog(LOG_INFO, "... passing to worker thread %d with %d users", wc->id, wc->client_count);
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
	++ uplink_connects.refcount;

	c->portaccount = & uplink_connects;
	
	return 0;
	
err:
	client_free(c);
	uplink_client = NULL;
	return -1;
}


/*
 *	Uplink thread
 */

void uplink_thread(void *asdf)
{
	sigset_t sigs_to_block;
	int rc;

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

	hlog(LOG_INFO, "Uplink_thread starting...");
		
	uplink_reconfiguring = 1;
	while (!uplink_shutting_down) {
		if (uplink_reconfiguring) {
			uplink_reconfiguring = 0;
			close_uplinkers();

			hlog(LOG_INFO, "Uplink thread ready.");
		}
		
		/* sleep for 1 second */
		poll(NULL, 0, 1000);
		
		if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
			hlog(LOG_ERR, "uplink_thread(): could not lock uplink_client_mutex: %s", strerror(rc));
			continue;
		}

		if (!uplink_client) {
			int n = 0;
			struct uplink_config_t *l = uplink_config;
			for (; l ; l = l->next )
			  ++n;
			l = uplink_config;
			if (n > 0) {
				n = random() % n;
				for (; l && n > 0; l = l->next, --n)
				  ;
				if (!l) l = uplink_config;
				if (l)
					make_uplink(l);
			}
		}

		if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
			hlog(LOG_ERR, "close_uplinkers(): could not unlock uplink_client_mutex: %s", strerror(rc));
			continue;
		}
	}
	
	hlog(LOG_DEBUG, "Uplinker thread shutting down uplinking sockets...");
	close_uplinkers();
}


/*
 *	Start / stop dupecheck
 */
void uplink_start(void)
{
	if (uplink_running)
		return;
	
	uplink_shutting_down = 0;
	
	if (pthread_create(&uplink_th, NULL, (void *)uplink_thread, NULL))
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
