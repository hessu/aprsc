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
#include "cfgfile.h"
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
#include "status.h"
#include "clientlist.h"
#include "client_heard.h"
#include "keyhash.h"
#include "tls.h"
#include "aprsis2.h"
#include "sctp.h"

#ifdef USE_SCTP
#include <netinet/sctp.h>
#endif

static struct listen_t *listen_list;

//  pthread_mutex_t mt_servercount = PTHREAD_MUTEX_INITIALIZER;

int accept_shutting_down;
int accept_reconfiguring;
time_t accept_reconfigure_after_tick = 0;

/* pseudoworker + pseudoclient for incoming UDP packets */
struct worker_t *udp_worker = NULL;
struct client_t *udp_pseudoclient = NULL;

/* structure allocator/free */

static struct listen_t *listener_alloc(void)
{
	struct listen_t *l = hmalloc(sizeof(*l));
	memset( l, 0, sizeof(*l) );
	l->fd = -1;
	l->id = -1;
	l->listener_id = -1;
	
	return l;
}

static void listener_free(struct listen_t *l)
{
	int i;
	
	hlog(LOG_DEBUG, "Freeing listener %d '%s': %s", l->id, l->name, l->addr_s);
	
	if (l->udp) {
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
	
	if (l->filter_s)
		hfree(l->filter_s);
	
	if (l->acl)
		acl_free(l->acl);
	
	/* merge listener list around this node */
	if (l->next)
		l->next->prevp = l->prevp;
	if (l->prevp)
		*(l->prevp) = l->next;
	
	hfree(l);
}

/*
 *	Copy / duplicate filters from listener config to an actual listener
 *	Free old filters if any are set.
 */

static void listener_copy_filters(struct listen_t *l, struct listen_config_t *lc)
{
	int i;
	char filter_s[FILTER_S_SIZE] = "";
	int filter_s_l = 0;
	
	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
		if (l->filters[i]) {
			hfree(l->filters[i]);
			l->filters[i] = NULL;
		}
		
		if (i < (sizeof(lc->filters)/sizeof(lc->filters[0]))) {
			if (!lc->filters[i])
				continue;
				
			l->filters[i] = hstrdup(lc->filters[i]);
			
			int len = strlen(l->filters[i]);
			if (filter_s_l + len + 2 < FILTER_S_SIZE) {
				if (filter_s_l)
					filter_s[filter_s_l++] = ' ';
				
				memcpy(filter_s + filter_s_l, l->filters[i], len);
				filter_s_l += len;
				filter_s[filter_s_l] = 0;
			}
		}
	}
	
	if (l->filter_s) {
		hfree(l->filter_s);
		l->filter_s = NULL;
	}
	
	if (filter_s_l == 0)
		return;
		
	sanitize_ascii_string(filter_s);
	l->filter_s = hstrdup(filter_s);
}

/*
 *	Open the TCP/SCTP listening socket
 */

static int open_tcp_listener(struct listen_t *l, const struct addrinfo *ai, char *which)
{
	int arg;
	int f;
	
	hlog(LOG_INFO, "Binding listening %s socket: %s", which, l->addr_s);
	
	if ((f = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
		hlog(LOG_CRIT, "socket(): %s\n", strerror(errno));
		return -1;
	}
	
	arg = 1;
	if (setsockopt(f, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg)) == -1)
		hlog(LOG_ERR, "setsockopt(%s, SO_REUSEADDR) failed for listener: %s", l->addr_s, strerror(errno));
#ifdef SO_REUSEPORT
	if (setsockopt(f, SOL_SOCKET, SO_REUSEPORT, (char *)&arg, sizeof(arg)) == -1)
		hlog(LOG_ERR, "setsockopt(%s, SO_REUSEPORT) failed for listener: %s", l->addr_s, strerror(errno));
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
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SO_REUSEADDR) failed: %s", l->addr_s, strerror(errno));
	}
#ifdef SO_REUSEPORT
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (char *)&arg, sizeof(arg)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SO_REUSEPORT) failed: %s", l->addr_s, strerror(errno));
	}
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

	if (1) {
		int len, arg;
		/* Set bigger socket buffer sizes for the UDP port..
		 * The current settings are quite large just to accommodate
		 * load testing at high packet rates without packet loss.
		 */
		len = sizeof(arg);
		arg = 64*1024; /* 20 Kbytes is good for about 5 seconds of APRS-IS full feed */
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &arg, len)) {
			hlog(LOG_ERR, "UDP listener setup: setsockopt(SO_RCVBUF, %d) failed: %s", arg, strerror(errno));
		}
		arg = 128*1024; /* This one needs to fit packets going to *all* UDP clients */
		if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &arg, len)) {
			hlog(LOG_ERR, "UDP listener setup: setsockopt(SO_SNDBUF, %d) failed: %s", arg, strerror(errno));
		}
	}

	/* set non-blocking mode */
	if (fcntl(c->fd, F_SETFL, O_NONBLOCK) == -1) {
		/* it really shouldn't fail.. and socket usage is "sendto(... MSG_DONTWAIT)",
		 * so it doesn't really even matter if it fails. */
		hlog(LOG_ERR, "UDP listener setup: fcntl(F_SETFL, O_NONBLOCK) failed: %s", strerror(errno));
	}
	
	l->udp = c;
	/* l->fd = fd -- except that close on it will kill working client setups.. */
	
	return fd;
}

static int open_listener(struct listen_config_t *lc)
{
	struct listen_t *l;
	int i;
	
	l = listener_alloc();
	l->id = lc->id;
	l->hidden = lc->hidden;
	l->corepeer = lc->corepeer;
	l->client_flags = lc->client_flags;
	l->clients_max = lc->clients_max;
	// Clamp listener maxclients with global MaxClients
	if (l->clients_max > maxclients) {
		hlog(LOG_WARNING, "Listener '%s' has maxclients %d which is higher than global MaxClients %d - using global limit", lc->name, l->clients_max, maxclients);
		l->clients_max = maxclients;
	}
	
	l->portaccount = port_accounter_alloc();
	
	/* Pick first of the AIs for this listen definition */
	l->addr_s = strsockaddr( lc->ai->ai_addr, lc->ai->ai_addrlen );
	l->name   = hstrdup(lc->name);
	l->portnum = lc->portnum;
	l->ai_protocol = lc->ai->ai_protocol;
	l->listener_id = keyhash(l->addr_s, strlen(l->addr_s), 0);
	l->listener_id = keyhash(&lc->ai->ai_socktype, sizeof(lc->ai->ai_socktype), l->listener_id);
	l->listener_id = keyhash(&lc->ai->ai_protocol, sizeof(lc->ai->ai_protocol), l->listener_id);
	hlog(LOG_DEBUG, "Opening listener %d/%d '%s': %s", lc->id, l->listener_id, lc->name, l->addr_s);
	
	if (lc->ai->ai_socktype == SOCK_DGRAM &&
	    lc->ai->ai_protocol == IPPROTO_UDP) {
		/* UDP listening is not quite same as TCP listening.. */
		i = open_udp_listener(l, lc->ai);
	} else if (lc->ai->ai_socktype == SOCK_STREAM && lc->ai->ai_protocol == IPPROTO_TCP) {
		/* TCP listening... */
		i = open_tcp_listener(l, lc->ai, "TCP");
#ifdef USE_SCTP
	} else if (lc->ai->ai_socktype == SOCK_STREAM &&
		   lc->ai->ai_protocol == IPPROTO_SCTP) {
		i = open_tcp_listener(l, lc->ai, "SCTP");
		if (i >= 0)
			i = sctp_set_listen_params(l);
#endif
	} else {
		hlog(LOG_ERR, "Unsupported listener protocol for '%s'", l->name);
		listener_free(l);
		return -1;
	}
	
	if (i < 0) {
		hlog(LOG_DEBUG, "... failed");
		listener_free(l);
		/* trigger reconfiguration after 30 seconds; probably an IP
		 * address that we tried to bind was not yet configured and
		 * it'll appear later in the boot process
		 */
		accept_reconfigure_after_tick = tick + 30;
		return -1;
	}
	
	hlog(LOG_DEBUG, "... ok, bound");
	
	/* Set up a TLS context if necessary */
#ifdef USE_SSL
	if (lc->keyfile && lc->certfile) {
		l->ssl = ssl_alloc();
		
		if (ssl_create(l->ssl, (void *)l)) {
			hlog(LOG_ERR, "Failed to create TLS context for '%s*': %s", lc->name, l->addr_s);
			listener_free(l);
			return -1;
		}
		
		if (ssl_certificate(l->ssl, lc->certfile, lc->keyfile)) {
			hlog(LOG_ERR, "Failed to load TLS key and certificates for '%s*': %s", lc->name, l->addr_s);
			listener_free(l);
			return -1;
		}
		
		/* optional client cert validation */
		if (lc->cafile) {
			if (ssl_ca_certificate(l->ssl, lc->cafile, 2)) {
				hlog(LOG_ERR, "Failed to load trusted TLS CA certificates for '%s*': %s", lc->name, l->addr_s);
				listener_free(l);
				return -1;
			}
		}
		
		hlog(LOG_INFO, "TLS initialized for '%s': %s%s", lc->name, l->addr_s, (lc->cafile) ? " (client validation enabled)" : "");
	}
#endif
	
	/* Copy access lists */
	if (lc->acl)
		l->acl = acl_dup(lc->acl);
	
	/* Copy filter definitions */
	listener_copy_filters(l, lc);
	
	hlog(LOG_DEBUG, "... adding %s to listened sockets", l->addr_s);
	// put (first) in the list of listening sockets
	l->next = listen_list;
	l->prevp = &listen_list;
	if (listen_list)
		listen_list->prevp = &l->next;
	listen_list = l;
	
	return 0;
}

static struct listen_t *find_listener_random_id(int id)
{
	struct listen_t *l = listen_list;
	
	while (l) {
		if (l->id == id)
			return l;
		l = l->next;
	}
	
	return NULL;
}

static struct listen_t *find_listener_hash_id(int id)
{
	struct listen_t *l = listen_list;
	
	while (l) {
		if (l->listener_id == id)
			return l;
		l = l->next;
	}
	
	return NULL;
}

static int rescan_client_acls(void)
{
	struct worker_t *w = worker_threads;
	struct client_t *c;
	struct listen_t *l;
	int pe;
	
	hlog(LOG_DEBUG, "Scanning old clients against new ACLs and listeners");
	
	while (w) {
		if ((pe = pthread_mutex_lock(&w->clients_mutex))) {
			hlog(LOG_ERR, "rescan_client_acls(worker %d): could not lock clients_mutex: %s", w->id, strerror(pe));
			return -1;
		}
		
		for (c = w->clients; (c); c = c->next) {
			/* do not disconnect uplinks at this point */
			if (!(c->flags & CLFLAGS_INPORT))
				continue;
			
			l = find_listener_hash_id(c->listener_id);
			if (!l) {
				/* listener is not there any more */
				hlog(LOG_INFO, "%s - Closing client on fd %d from %s (listener has been removed)", c->addr_loc, c->fd, c->addr_rem);
				shutdown(c->fd, SHUT_RDWR);
				continue;
			}
			
			/* is there an acl? */
			if (!l->acl)
				continue;
				
			/* there is, check */
			if (!acl_check(l->acl, (struct sockaddr *)&c->addr, sizeof(c->addr))) {
				hlog(LOG_INFO, "%s - Denying client on fd %d from %s (new ACL)", c->addr_loc, c->fd, c->addr_rem);
				shutdown(c->fd, SHUT_RDWR);
				continue;
			}
		}
		
		if ((pe = pthread_mutex_unlock(&w->clients_mutex))) {
			hlog(LOG_ERR, "rescan_client_acls(worker %d): could not unlock clients_mutex: %s", w->id, strerror(pe));
			/* we'd going to deadlock here... */
			exit(1);
		}
		
		w = w->next;
	}
	

	return 0;
}


static void listener_update_config(struct listen_t *l, struct listen_config_t *lc)
{
	hlog(LOG_DEBUG, "listener_update_config: %d '%s': %s:%d", lc->id, lc->name, lc->host, lc->portnum);
	
	/* basic flags which can be changed on the fly */
	l->clients_max = lc->clients_max; /* could drop clients when decreasing maxclients (done in worker) */
	l->hidden = lc->hidden; /* could mark old clients on port hidden, too - needs to be done in worker */
	l->client_flags = lc->client_flags; /* this one must not change old clients */
	
	/* Filters */
	listener_copy_filters(l, lc);
	
	/* ACLs */
	if (l->acl) {
		acl_free(l->acl);
		l->acl = NULL;
	}
	if (lc->acl) {
		l->acl = acl_dup(lc->acl);
	}
	
	/* Listen address change? Rebind? */
}

static int open_missing_listeners(void)
{
	struct listen_config_t *lc;
	struct listen_t *l;
	int failed = 0;
	
	for (lc = listen_config; (lc); lc = lc->next) {
		if ((l = find_listener_random_id(lc->id))) {
			hlog(LOG_DEBUG, "open_missing_listeners: already listening %d '%s': %s:%d", lc->id, lc->name, lc->host, lc->portnum);
			listener_update_config(l, lc);
			continue;
		}
		
		if (open_listener(lc) != 0)
			failed++;
	}
	
	return failed;
}

static int close_removed_listeners(void)
{
	int closed = 0;
		
	hlog(LOG_DEBUG, "Closing removed listening sockets...");
	struct listen_t *l, *next;
	next = listen_list;
	while (next) {
		l = next;
		next = l->next;
		
		struct listen_config_t *lc = find_listen_config_id(listen_config, l->id);
		if (!lc) {
			hlog(LOG_INFO, "Listener %d (%s) no longer in configuration, closing port...",
				l->id, l->addr_s);
			listener_free(l);
			closed++;
		}
	}

	if (closed)
		hlog(LOG_INFO, "Closed %d removed listeners.", closed);

	return closed;
}

static void close_listeners(void)
{
	if (!listen_list)
		return;
		
	hlog(LOG_DEBUG, "Closing listening sockets....");
	while (listen_list)
		listener_free(listen_list);
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
		c->ai_protocol = IPPROTO_UDP;
		c->portnum = pe->local_port; // local port
		c->state = CSTATE_COREPEER;
		c->validated = VALIDATED_WEAK;
		c->flags = CLFLAGS_UPLINKPORT;
		c->handler_line_in = &incoming_handler;
		memcpy((void *)&c->udpaddr.sa, (void *)pe->ai->ai_addr, pe->ai->ai_addrlen);
		c->udpaddrlen = pe->ai->ai_addrlen;
		c->udp_port = pe->remote_port; // remote port
		c->addr = c->udpaddr;
		c->udpclient = udpclient;
		//c->portaccount = l->portaccount;
		c->keepalive = tick + keepalive_interval;
		c->last_read = tick; /* not simulated time */
		
		inbound_connects_account(3, c->udpclient->portaccount); /* "3" = udp, not listening..  */
		
		/* set up peer serverid to username */
		strncpy(c->username, pe->serverid, sizeof(c->username));
		c->username[sizeof(c->username)-1] = 0;
		c->username_len = strlen(c->username);
		
		/* convert client address to string */
		s = strsockaddr( &c->udpaddr.sa, c->udpaddrlen );
		
		/* text format of client's IP address + port */
		strncpy(c->addr_rem, s, sizeof(c->addr_rem));
		c->addr_rem[sizeof(c->addr_rem)-1] = 0;
		hfree(s);
		
		/* hex format of client's IP address + port */
		s = hexsockaddr( &c->udpaddr.sa, c->udpaddrlen );
		
		strncpy(c->addr_hex, s, sizeof(c->addr_hex));
		c->addr_hex[sizeof(c->addr_hex)-1] = 0;
		hfree(s);

		/* text format of servers' connected IP address + port */
		addr_len = sizeof(sa);
		if (getsockname(c->udpclient->fd, &sa.sa, &addr_len) == 0) { /* Fails very rarely.. */
			/* present my socket end address as a malloced string... */
			s = strsockaddr( &sa.sa, addr_len );
		} else {
			hlog(LOG_ERR, "Peer config: getsockname on udpclient->fd failed: %s", strerror(errno));
			s = hstrdup( "um" ); /* Server's bound IP address.. TODO: what? */
		}
		strncpy(c->addr_loc, s, sizeof(c->addr_loc));
		c->addr_loc[sizeof(c->addr_loc)-1] = 0;
		hfree(s);

		/* pass the client to the first worker thread */
		if (pass_client_to_worker(worker_threads, c)) {
			hlog(LOG_ERR, "Failed to pass UDP peer %s (%s) to worker", pe->name, pe->host);
			client_free(c);
		}
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
	
	if (pass_client_to_worker(worker_threads, c)) {
		hlog(LOG_ERR, "Failed to pass magic peerip_clients_close message pseudoclient to worker");
		client_free(c);
	}
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
	validated = http_udp_upload_login(remote_host, login_string, &username, "UDP submit");
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
	
	udp_pseudoclient->portaccount = l->portaccount;
	e = pseudoclient_push_packet(udp_worker, udp_pseudoclient, username, packet, packet_len);
	clientaccount_add_rx(udp_pseudoclient, IPPROTO_UDP, len, 1, (e < 0) ? e : 0, 0);
	udp_pseudoclient->portaccount = NULL;
	
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
		if (!(l->client_flags & CLFLAGS_UDPSUBMIT)) {
			hlog(LOG_DEBUG, "accept thread discarded an UDP packet on a listening socket");
			continue;
		}
		
		addrs = strsockaddr(&addr.sa, addrlen);
		accept_process_udpsubmit(l, buf, i, addrs);
		hfree(addrs);
	}
}

/*
 *	Accept a single client
 */

struct client_t *accept_client_for_listener(struct listen_t *l, int fd, char *addr_s, union sockaddr_u *sa, unsigned addr_len)
{
	struct client_t *c;
	char *s;
	int i;
	union sockaddr_u sa_loc; /* local address */
	socklen_t addr_len_loc = sizeof(sa_loc);
	
	c = client_alloc();
	if (!c)
		return NULL;
		
	c->fd    = fd;
	c->listener_id = l->listener_id;
	c->addr  = *sa;
	c->ai_protocol = l->ai_protocol;
	c->portnum = l->portnum;
	c->hidden  = l->hidden;
	c->flags   = l->client_flags;
	c->udpclient = client_udp_find(udpclients, sa->sa.sa_family, l->portnum);
	c->portaccount = l->portaccount;
	c->last_read = tick; /* not simulated time */
	inbound_connects_account(1, c->portaccount); /* account all ports + port-specifics */
	
	/* text format of client's IP address + port */
	strncpy(c->addr_rem, addr_s, sizeof(c->addr_rem));
	c->addr_rem[sizeof(c->addr_rem)-1] = 0;

	/* hex format of client's IP address + port */
	s = hexsockaddr( &sa->sa, addr_len );
	strncpy(c->addr_hex, s, sizeof(c->addr_hex));
	c->addr_hex[sizeof(c->addr_hex)-1] = 0;
	hfree(s);

	/* text format of servers' connected IP address + port */
	if (getsockname(fd, &sa_loc.sa, &addr_len_loc) == 0) { /* Fails very rarely.. */
		if (addr_len_loc > sizeof(sa_loc))
			hlog(LOG_ERR, "accept_client_for_listener: getsockname for client %s truncated local address of %d to %ld bytes", c->addr_rem, addr_len_loc, sizeof(sa_loc));
		/* present my socket end address as a malloced string... */
		s = strsockaddr( &sa_loc.sa, addr_len_loc );
	} else {
		s = hstrdup( l->addr_s ); /* Server's bound IP address */
		hlog(LOG_ERR, "accept_client_for_listener: getsockname for client %s failed: %s (using '%s' instead)", c->addr_rem, strerror(errno), s);
	}
	strncpy(c->addr_loc, s, sizeof(c->addr_loc));
	c->addr_loc[sizeof(c->addr_loc)-1] = 0;
	hfree(s);

	/* apply predefined filters */
	for (i = 0; i < (sizeof(l->filters)/sizeof(l->filters[0])); ++i) {
		if (l->filters[i]) {
			if (filter_parse(c, l->filters[i], 0) < 0) { /* system filters */
				hlog(LOG_ERR, "Bad system filter definition: %s", l->filters[i]);
			}
		}
	}
	if (l->filter_s) {
		strncpy(c->filter_s, l->filter_s, sizeof(c->filter_s));
		c->filter_s[FILTER_S_SIZE-1] = 0;
	}
	
	return c;
}

/*
 *	Pick next worker to feed a client to
 */

static struct worker_t *pick_next_worker(void)
{
	static int next_receiving_worker;
	struct worker_t *w, *wc;
	int i;

#if 1
	/* Use simple round-robin on client feeding.  Least clients is
	 * quite attractive idea, but when clients arrive at huge bursts
	 * they tend to move in big bunches, and it takes quite some while
	 * before the worker updates its client-counters.
	 */
	for (i = 0, w = worker_threads; w ;  w = w->next, i++) {
		if ( i >= next_receiving_worker)
			break;
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
	return wc;
}

/*
 *	Accept a single connection
 */

static void do_accept(struct listen_t *l)
{
	int fd;
	struct client_t *c;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len = sizeof(sa);
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
	
	/* Limit amount of connections per port, and globally.
	 * Error messages written just before closing the socet may or may not get
	 * to the user, but at least we try.
	 */
	if (l->portaccount->gauge >= l->clients_max || inbound_connects.gauge >= maxclients) {
		if (inbound_connects.gauge >= maxclients) {
			hlog(LOG_INFO, "%s - Denied client on fd %d from %s: MaxClients reached (%ld)", l->addr_s, fd, s, inbound_connects.gauge);
			/* The "if" is here only to silence a compiler warning
			 * about ignoring the result value. We're really
			 * disconnecting the client right now, so we don't care.
			 */
			if (write(fd, "# Server full\r\n", 15)) {};
		} else {
			hlog(LOG_INFO, "%s - Denied client on fd %d from %s: Too many clients on Listener (%ld)", l->addr_s, fd, s, l->portaccount->gauge);
			if (write(fd, "# Port full\r\n", 13)) {};
		}
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
	
	c = accept_client_for_listener(l, fd, s, &sa, addr_len);
	if (!c) {
		hlog(LOG_ERR, "%s - client_alloc returned NULL, too many clients. Denied client on fd %d from %s", l->addr_s, fd, s);
		close(fd);
		hfree(s);
		inbound_connects_account(-1, l->portaccount); /* account rejected connection */
		return;
	}
	hfree(s);

	c->state   = CSTATE_LOGIN;
	c->keepalive = tick + keepalive_interval;
	
	/* use the default login handler */
	if (c->flags & CLFLAGS_IS2)
		c->is2_input_handler = &is2_input_handler_login;
	else
		c->handler_line_in = &login_handler;

#ifdef USE_SSL
	if (l->ssl) {
		if (ssl_create_connection(l->ssl, c, 0)) {
			close(fd);
			inbound_connects_account(-1, l->portaccount); /* account rejected connection */
			return;
		}
	}
#endif
	
	hlog(LOG_DEBUG, "%s - Accepted client on fd %d from %s", c->addr_loc, c->fd, c->addr_rem);
	
	/* set client socket options, return -1 on serious errors */
	if (set_client_sockopt(c) != 0)
		goto err;
	
	/* ok, found it... lock the new client queue and pass the client */
	if (pass_client_to_worker(pick_next_worker(), c))
		goto err;
	
	return;
	
err:

	inbound_connects_account(0, c->portaccount); /* something failed, remove this from accounts.. */
	client_free(c);
	return;
}

/*
 *	Find a listener which this client is connected on
 */

struct listen_t *liveupgrade_find_listener(int listener_id)
{
	struct listen_t *l;
	
	l = listen_list;
	
	while (l) {
		if (l->listener_id == listener_id)
			break;
		l = l->next;
	}
	
	return l;
}

/*
 *	Map old rxerrs counter values to new ones based on the string mapping
 */

static int *accept_rx_err_map(cJSON *rx_err_labels, int *old_rxerrs_len)
{
	int *rxerr_map = NULL;
	int i, j;

	*old_rxerrs_len = cJSON_GetArraySize(rx_err_labels);
	
	if (*old_rxerrs_len > 0) {
		rxerr_map = hmalloc(sizeof(*rxerr_map) * *old_rxerrs_len);
		for (i = 0; i < *old_rxerrs_len; i++) {
			rxerr_map[i] = -1; // default: no mapping
			
			cJSON *rxerr = cJSON_GetArrayItem(rx_err_labels, i);
			if (!rxerr || rxerr->type != cJSON_String)
				continue;
			
			for (j = 0; j < INERR_BUCKETS; j++) {
				if (strcmp(inerr_labels[j], rxerr->valuestring) == 0) {
					//hlog(LOG_DEBUG, "Mapped old rxerr index %d with new index %d: %s", i, j, rxerr->valuestring);
					rxerr_map[i] = j;
				}
			}
		}
	}
	
	return rxerr_map;
}

static void accept_rx_err_load(struct client_t *c, cJSON *rx_errs, int *rxerr_map, int rxerr_map_len)
{
	int i;
	int alen = cJSON_GetArraySize(rx_errs);
	
	for (i = 0; i < rxerr_map_len && i < alen; i++) {
		if (rxerr_map[i] >= 0 && rxerr_map[i] < INERR_BUCKETS) {
			cJSON *val = cJSON_GetArrayItem(rx_errs, i);
			if ((val) && val->type == cJSON_Number && val->valuedouble > 0)
				c->localaccount.rxerrs[rxerr_map[i]] = val->valuedouble;
		}
	}
}

/*
 *	Live upgrade: accept old clients
 */

static cJSON *accept_liveupgrade_cJSON_get(cJSON *tree, const char *key, int type, const char *logid)
{
	cJSON *val;
	
	val = cJSON_GetObjectItem(tree, key);
	
	if (!val) {
		hlog(LOG_ERR, "Live upgrade: Client '%s' JSON: Field '%s' missing", logid, key);
		return NULL;
	}
	
	if (val->type != type) {
		hlog(LOG_ERR, "Live upgrade: Client '%s' JSON: Field '%s' has incorrect type %d, expected %d", logid, key, val->type, type);
		return NULL;
	}
	
	return val;
}

static int accept_liveupgrade_single(cJSON *client, int *rxerr_map, int rxerr_map_len)
{
	cJSON *fd, *listener_id, *username, *time_connect, *tick_connect;
	cJSON *state;
	cJSON *link;
	cJSON *addr_loc;
	cJSON *udp_port;
	cJSON *app_name, *app_version;
	cJSON *verified;
	cJSON *obuf_q;
	cJSON *bytes_rx, *bytes_tx;
	cJSON *pkts_rx, *pkts_tx, *pkts_ign;
	cJSON *rx_errs;
	cJSON *filter;
	cJSON *ibuf, *obuf;
	cJSON *client_heard;
	cJSON *lat, *lng;
	unsigned addr_len;
	union sockaddr_u sa;
	char *argv[256];
	int i, argc;
	const char *username_s = "unknown";
	
	/* get username first, so we can log it later */
	username = accept_liveupgrade_cJSON_get(client, "username", cJSON_String, username_s);
	if (username)
		username_s = username->valuestring;
	
	fd = accept_liveupgrade_cJSON_get(client, "fd", cJSON_Number, username_s);
	int fd_i = -1;
	if (fd)
		fd_i = fd->valueint;
		
	if (fd_i < 0) {
		hlog(LOG_INFO, "Live upgrade: Client '%s' has negative fd %d, ignoring (corepeer?)", username_s, fd_i);
		return -1;
	}
	
	listener_id = accept_liveupgrade_cJSON_get(client, "listener_id", cJSON_Number, username_s);
	state = accept_liveupgrade_cJSON_get(client, "state", cJSON_String, username_s);
	time_connect = accept_liveupgrade_cJSON_get(client, "t_connect", cJSON_Number, username_s);
	addr_loc = accept_liveupgrade_cJSON_get(client, "addr_loc", cJSON_String, username_s);
	app_name = accept_liveupgrade_cJSON_get(client, "app_name", cJSON_String, username_s);
	app_version = accept_liveupgrade_cJSON_get(client, "app_version", cJSON_String, username_s);
	verified = accept_liveupgrade_cJSON_get(client, "verified", cJSON_Number, username_s);
	obuf_q = accept_liveupgrade_cJSON_get(client, "obuf_q", cJSON_Number, username_s);
	bytes_rx = accept_liveupgrade_cJSON_get(client, "bytes_rx", cJSON_Number, username_s);
	bytes_tx = accept_liveupgrade_cJSON_get(client, "bytes_tx", cJSON_Number, username_s);
	pkts_rx = accept_liveupgrade_cJSON_get(client, "pkts_rx", cJSON_Number, username_s);
	pkts_tx = accept_liveupgrade_cJSON_get(client, "pkts_tx", cJSON_Number, username_s);
	pkts_ign = accept_liveupgrade_cJSON_get(client, "pkts_ign", cJSON_Number, username_s);
	rx_errs = accept_liveupgrade_cJSON_get(client, "rx_errs", cJSON_Array, username_s);
	filter = accept_liveupgrade_cJSON_get(client, "filter", cJSON_String, username_s);
	
	/* optional */
	tick_connect = cJSON_GetObjectItem(client, "t_connect_tick");
	udp_port = cJSON_GetObjectItem(client, "udp_port");
	ibuf = cJSON_GetObjectItem(client, "ibuf");
	obuf = cJSON_GetObjectItem(client, "obuf");
	client_heard = cJSON_GetObjectItem(client, "client_heard");
	lat = cJSON_GetObjectItem(client, "lat");
	lng = cJSON_GetObjectItem(client, "lng");
	link = cJSON_GetObjectItem(client, "link");
	
	if (!(
		(fd)
		&& (listener_id)
		&& (state)
		&& (username)
		&& (time_connect)
		&& (addr_loc)
		&& (app_name)
		&& (app_version)
		&& (verified)
		&& (obuf_q)
		&& (bytes_rx)
		&& (bytes_tx)
		&& (pkts_rx)
		&& (pkts_tx)
		&& (pkts_ign)
		&& (rx_errs)
		&& (filter)
		)) {
			hlog(LOG_ERR, "Live upgrade: Fields missing from client JSON, discarding client fd %d", fd_i);
			if (fd_i >= 0)
				close(fd_i);
			return -1;
	}
	
	hlog(LOG_DEBUG, "Old client on fd %d: %s", fd->valueint, username->valuestring);
	
	/* fetch peer address from the fd instead of parsing it from text */
	addr_len = sizeof(sa);
	if (getpeername(fd->valueint, &sa.sa, &addr_len) != 0) {
		/* Sometimes clients disconnect during upgrade, especially on slow RPi servers... */
		if (errno == ENOTCONN)
			hlog(LOG_INFO, "Live upgrade: Client %s on fd %d has disconnected during upgrade (%s)",
				username->valuestring, fd->valueint, strerror(errno));
		else
			hlog(LOG_ERR, "Live upgrade: getpeername client fd %d failed: %s", fd->valueint, strerror(errno));
		close(fd->valueint);
		return -1;
	}
	
	/* convert client address to string */
	char *client_addr_s = strsockaddr( &sa.sa, addr_len );
	
	/* find the right listener for this client, for configuration and accounting */
	struct listen_t *l = liveupgrade_find_listener(listener_id->valueint);
	if (!l) {
		hlog(LOG_INFO, "Live upgrade: Listener has been removed for fd %d (%s - local %s): disconnecting %s",
			fd->valueint, client_addr_s, addr_loc->valuestring, username->valuestring);
		close(fd->valueint);
		hfree(client_addr_s);
		return -1;
	}
	
	struct client_t *c = accept_client_for_listener(l, fd->valueint, client_addr_s, &sa, addr_len);
	if (!c) {
		hlog(LOG_ERR, "Live upgrade - client_alloc returned NULL, too many clients. Denied client %s on fd %d from %s",
			username->valuestring, fd->valueint, client_addr_s);
		close(fd->valueint);
		hfree(client_addr_s);
		return -1;
	}
	
	hfree(client_addr_s);
	
	if ((link) && (link->valuestring) && strcmp(link->valuestring, "is2") == 0) {
		c->flags |= CLFLAGS_IS2;
	}
	
	if (strcmp(state->valuestring, "connected") == 0) {
		c->state   = CSTATE_CONNECTED;

		/* use the default login handler */
		if (c->flags & CLFLAGS_IS2)
			c->is2_input_handler = &is2_input_handler;
		else
			c->handler_line_in = &incoming_handler;

		strncpy(c->username, username->valuestring, sizeof(c->username));
		c->username[sizeof(c->username)-1] = 0;
		c->username_len = strlen(c->username);
	} else if (strcmp(state->valuestring, "login") == 0) {
		c->state   = CSTATE_LOGIN;
		
		/* use the default login handler */
		if (c->flags & CLFLAGS_IS2)
			c->is2_input_handler = &is2_input_handler_login;
		else
			c->handler_line_in = &login_handler;
	} else {
		hlog(LOG_ERR, "Live upgrade: Client %s is in invalid state '%s' (fd %d)", l->addr_s, state->valuestring, l->fd);
		goto err;
	}
	/* distribute keepalive intervals for the existing old clients
	 * but send them rather sooner than later */
	// coverity[dont_call]  // squelch warning: not security sensitive use of random(): load distribution
	c->keepalive = tick + (random() % (keepalive_interval/2));
	/* distribute cleanup intervals over the next 2 minutes */
	// coverity[dont_call]  // squelch warning: not security sensitive use of random(): load distribution
	c->cleanup = tick + (random() % 120);
	
	c->connect_time = time_connect->valueint;
	/* live upgrade / backward compatibility: upgrading from <= 1.8.2 requires the 'else' path' */
	if (tick_connect && tick_connect->type == cJSON_Number)
		c->connect_tick = tick_connect->valueint;
	else /* convert to monotonic time */
		c->connect_tick = tick - (now - c->connect_time);
	
	c->validated = verified->valueint;
	c->localaccount.rxbytes = bytes_rx->valuedouble;
	c->localaccount.txbytes = bytes_tx->valuedouble;
	c->localaccount.rxpackets = pkts_rx->valuedouble;
	c->localaccount.txpackets = pkts_tx->valuedouble;
	c->localaccount.rxdrops = pkts_ign->valuedouble;
	
	login_set_app_name(c, app_name->valuestring, app_version->valuestring);
	
	// handle client's filter setting
	if (c->flags & CLFLAGS_USERFILTEROK && (filter) && (filter->valuestring) && *(filter->valuestring)) {
		// archive a copy of the filters, for status display
		strncpy(c->filter_s, filter->valuestring, FILTER_S_SIZE);
		c->filter_s[FILTER_S_SIZE-1] = 0;
		sanitize_ascii_string(c->filter_s);
		
		char *f = hstrdup(filter->valuestring);
		argc = parse_args(argv, f);
		for (i = 0; i < argc; ++i) {
			filter_parse(c, argv[i], 1);
		}
		hfree(f);
	}
	
	// set up UDP downstream if necessary
	if (udp_port && udp_port->type == cJSON_Number && udp_port->valueint > 1024 && udp_port->valueint < 65536) {
		if (login_setup_udp_feed(c, udp_port->valueint) != 0) {
			hlog(LOG_DEBUG, "%s/%s: Requested UDP on client port with no UDP configured", c->addr_rem, c->username);
		}
	}
	
	// fill up ibuf
	if (ibuf && ibuf->type == cJSON_String && ibuf->valuestring) {
		int l = hex_decode(c->ibuf, c->ibuf_size, ibuf->valuestring);
		if (l < 0) {
			hlog(LOG_ERR, "Live upgrade: %s/%s: Failed to decode ibuf: %s", c->addr_rem, c->username, ibuf->valuestring);
		} else {
			c->ibuf_end = l;
			hlog(LOG_DEBUG, "Live upgrade: Decoded ibuf %d bytes: '%.*s'", l, l, c->ibuf);
			hlog(LOG_DEBUG, "Hex: %s", ibuf->valuestring);
		}
	}
	
	// fill up obuf
	if (obuf && obuf->type == cJSON_String && obuf->valuestring) {
		int l = hex_decode(c->obuf, c->obuf_size, obuf->valuestring);
		if (l < 0) {
			hlog(LOG_ERR, "Live upgrade: %s/%s: Failed to decode obuf: %s", c->addr_rem, c->username, obuf->valuestring);
		} else {
			c->obuf_start = 0;
			c->obuf_end = l;
			hlog(LOG_DEBUG, "Live upgrade: Decoded obuf %d bytes: '%.*s'", l, l, c->obuf);
			hlog(LOG_DEBUG, "Hex: %s", obuf->valuestring);
		}
	}
	
	/* load list of stations heard by this client, to immediately support
	 * messaging
	 */
	if (client_heard && client_heard->type == cJSON_Array)
		client_heard_json_load(c, client_heard);
	
	/* load rxerrs counters, with error name string mapping to support
	 * adding/reordering of error counters
	 */
	if (rx_errs && rx_errs->type == cJSON_Array && rxerr_map && rxerr_map_len > 0)
		accept_rx_err_load(c, rx_errs, rxerr_map, rxerr_map_len);
	
	/* set client lat/lon, if they're given
	 */
	if (lat && lng && lat->type == cJSON_Number && lng->type == cJSON_Number) {
		c->loc_known = 1;
		c->lat = lat->valuedouble;
		c->lng = lng->valuedouble;
	}
	
	hlog(LOG_DEBUG, "%s - Accepted live upgrade client on fd %d from %s", c->addr_loc, c->fd, c->addr_rem);
	
	/* set client socket options, return -1 on serious errors */
	if (set_client_sockopt(c) != 0)
		goto err;
	
	/* Add the client to the client list. */
	int old_fd = clientlist_add(c);
	if (c->validated && old_fd != -1) {
		/* TODO: If old connection is TLS validated, and this one is not, do not disconnect it. */
		hlog(LOG_INFO, "fd %d: Disconnecting duplicate validated client with username '%s'", old_fd, c->username);
		shutdown(old_fd, SHUT_RDWR);
	}
	
	/* ok, found it... lock the new client queue and pass the client */
	if (pass_client_to_worker(pick_next_worker(), c))
		goto err;
	
	return 0;
	
err:
	close(c->fd);
	inbound_connects_account(0, c->portaccount); /* something failed, remove this from accounts.. */
	client_free(c);
	return -1;
}

static void accept_liveupgrade_accept(void)
{
	int clen, i;
	int accepted = 0;
	int old_rxerrs_len = 0;
	int *rxerr_map = NULL;
	
	hlog(LOG_INFO, "Accept: Collecting live upgrade clients...");
	
	/* Create mapping for rx_errs table indexes, so that rxerrs can be
	 * loaded right even if new ones were added in the middle, or if
	 * the error counters were reordered
	 */
	cJSON *rx_err_labels = cJSON_GetObjectItem(liveupgrade_status, "rx_errs");
	if ((rx_err_labels) && rx_err_labels->type == cJSON_Array)
		rxerr_map = accept_rx_err_map(rx_err_labels, &old_rxerrs_len);
	
	cJSON *clients = cJSON_GetObjectItem(liveupgrade_status, "clients");
	if (!clients || clients->type != cJSON_Array) {
		hlog(LOG_ERR, "Accept: Live upgrade JSON does not contain 'clients' array!");
	} else {
		clen = cJSON_GetArraySize(clients);
		hlog(LOG_DEBUG, "Clients array length %d", clen);
		for (i = 0; i < clen; i++) {
			cJSON *client = cJSON_GetArrayItem(clients, i);
			if (!client || client->type != cJSON_Object) {
				hlog(LOG_ERR, "Accept: Live upgrade JSON file, get client %d failed", i);
				continue;
			}
			if (accept_liveupgrade_single(client, rxerr_map, old_rxerrs_len) == 0)
				accepted++;
		}
		hlog(LOG_INFO, "Accepted %d of %d old clients in live upgrade", accepted, clen);
		if (accepted != clen)
			hlog(LOG_ERR, "Live upgrade: Failed to accept %d old clients, see above for reasons", clen-accepted);
	}
	
	cJSON_Delete(liveupgrade_status);
	liveupgrade_status = NULL;
	
	if (rxerr_map)
		hfree(rxerr_map);
}

/*
 *	Accept thread
 */

void accept_thread(void *asdf)
{
	sigset_t sigs_to_block;
	int e, n;
	struct pollfd *acceptpfd = NULL;
	struct listen_t **acceptpl = NULL;
	int poll_n = 0;
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
	hlog(LOG_INFO, "Accept thread starting...");
	
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
			close_removed_listeners();
			
			/* start listening on the sockets */
			int failed_listeners = open_missing_listeners();
			
			if (failed_listeners > 0) {
				hlog(LOG_CRIT, "Failed to listen on %d configured listeners.", failed_listeners);
				exit(2);
			}
			
			/* reconfiguration must scan old clients against ACL */
			rescan_client_acls();
			
			/* how many are we polling */
			poll_n = 0;
			for (l = listen_list; (l); l = l->next)
				if (!l->corepeer)
					poll_n++;
			
			hlog(LOG_DEBUG, "Generating polling list for %d listeners...", poll_n);
			
			/* array of FDs for poll() */
			if (acceptpfd)
				hfree(acceptpfd);
			acceptpfd = hmalloc(poll_n * sizeof(*acceptpfd));
			
			/* array of listeners */
			if (acceptpl)
				hfree(acceptpl);
			acceptpl = hmalloc(poll_n * sizeof(*acceptpl));
			
			n = 0;
			int has_filtered_listeners_now = 0;
			for (l = listen_list; (l); l = l->next) {
				/* The accept thread does not poll() UDP sockets for core peers.
				 * Worker 0 takes care of that, and processes the incoming packets.
				 */
				if (l->corepeer) {
					hlog(LOG_DEBUG, "... %d: fd %d (%s) - not polled, is corepeer", n, (l->udp) ? l->udp->fd : l->fd, l->addr_s);
					continue;
				}
				
				int fd;
				if (l->udp) {
					l->udp->polled = 1;
					fd = l->udp->fd;
				} else {
					fd = l->fd;
				}
				
				if ((l->filter_s) || (l->client_flags & CLFLAGS_USERFILTEROK))
					has_filtered_listeners_now = 1;
				
				hlog(LOG_DEBUG, "... %d: fd %d (%s)", n, fd, l->addr_s);
				acceptpfd[n].fd = fd;
				acceptpfd[n].events = POLLIN|POLLPRI|POLLERR|POLLHUP;
				acceptpl[n] = l;
				n++;
			}
			hlog(LOG_INFO, "Accept thread ready.");
			have_filtered_listeners = has_filtered_listeners_now;
			if (!have_filtered_listeners)
				hlog(LOG_INFO, "Disabled historydb, listeners do not have filtering enabled.");
			
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
			
			/*
			 * generate UDP peer clients
			 */
			peerip_clients_close();
			if (peerip_config)
				peerip_clients_config();
			
			/* accept liveupgrade clients */
			if (liveupgrade_status)
				accept_liveupgrade_accept();
		} else if (accept_reconfigure_after_tick != 0 && accept_reconfigure_after_tick <= tick) {
			hlog(LOG_INFO, "Trying to reconfigure listeners due to a previous failure");
			accept_reconfiguring = 1;
			accept_reconfigure_after_tick = 0;
		}
		
		/* check for new connections */
		e = poll(acceptpfd, poll_n, 200);
		if (e == 0)
			continue;
		if (e < 0) {
			if (errno == EINTR)
				continue;
			hlog(LOG_ERR, "poll() on accept failed: %s (continuing)", strerror(errno));
			continue;
		}
		
		/* now, which socket was that on? */
		for (n = 0; n < poll_n; n++) {
			l = acceptpl[n];
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
		}
	}
	
	if (accept_shutting_down == 2)
		worker_shutdown_clients = cJSON_CreateArray();
	
	hlog(LOG_DEBUG, "Accept thread shutting down listening sockets and worker threads...");
	uplink_stop();
	close_listeners();
	dupecheck_stop();
	http_shutting_down = 1;
	workers_stop(accept_shutting_down);
	hfree(acceptpfd);
	hfree(acceptpl);
	acceptpfd = NULL;
	acceptpl = NULL;
	
	/* free up the pseudo-client */
	client_free(udp_pseudoclient);
	udp_pseudoclient = NULL;
	
	/* free up the pseudo-worker structure, after dupecheck is long dead */
	worker_free_buffers(udp_worker);
	hfree(udp_worker);
	udp_worker = NULL;
}

/*
 *	generate status information in status.json about the listeners
 */

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
		cJSON_AddNumberToObject(jl, "id", l->id);
		cJSON_AddStringToObject(jl, "name", l->name);
		if (l->ai_protocol == IPPROTO_SCTP)
			cJSON_AddStringToObject(jl, "proto", "sctp");
		else
			cJSON_AddStringToObject(jl, "proto", (l->udp) ? "udp" : "tcp");
		cJSON_AddStringToObject(jl, "addr", l->addr_s);
		if (l->filter_s)
			cJSON_AddStringToObject(jl, "filter", l->filter_s);
		cJSON_AddNumberToObject(jl, "clients", l->portaccount->gauge);
		cJSON_AddNumberToObject(jl, "clients_peak", l->portaccount->gauge_max);
		cJSON_AddNumberToObject(jl, "clients_max", l->clients_max);
		cJSON_AddNumberToObject(jl, "connects", l->portaccount->counter);
		cJSON_AddNumberToObject(jl, "bytes_rx", l->portaccount->rxbytes);
		cJSON_AddNumberToObject(jl, "bytes_tx", l->portaccount->txbytes);
		cJSON_AddNumberToObject(jl, "pkts_rx", l->portaccount->rxpackets);
		cJSON_AddNumberToObject(jl, "pkts_tx", l->portaccount->txpackets);
		cJSON_AddNumberToObject(jl, "pkts_ign", l->portaccount->rxdrops);
		cJSON_AddNumberToObject(jl, "pkts_dup", l->portaccount->rxdupes);
		json_add_rxerrs(jl, "rx_errs", l->portaccount->rxerrs);
		cJSON_AddItemToArray(listeners, jl);
		
		if (!(l->udp)) {
			total_clients += l->portaccount->gauge;
			total_connects += l->portaccount->counter;
		}
		/*
		total_rxbytes += l->portaccount->rxbytes;
		total_txbytes += l->portaccount->txbytes;
		total_rxpackets += l->portaccount->rxpackets;
		total_txpackets += l->portaccount->txpackets;
		*/
	}
	
	cJSON_AddNumberToObject(totals, "clients_max", maxclients);
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
