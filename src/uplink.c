/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *	(c) Heikki Hannikainen, OH7LZB
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

/*
 *	uplink thread: create uplink connections as necessary
 *	(and tear them down)
 */

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <resolv.h>

#include "config.h"
#include "version.h"
#include "status.h"
#include "uplink.h"
#include "hmalloc.h"
#include "hlog.h"
#include "worker.h"
#include "login.h"
#include "incoming.h"
#include "outgoing.h"
#include "filter.h"
#include "tls.h"

int uplink_reconfiguring;
int uplink_shutting_down;

/* currently-connected uplinks */
struct uplink_client_t {
	struct client_t *client;
	struct uplink_client_t *next;
	struct uplink_client_t **prevp;
} *uplink_clients;

pthread_mutex_t uplink_client_mutex = PTHREAD_MUTEX_INITIALIZER;

int uplink_running;
pthread_t uplink_th;

struct uplink_config_t *uplink_config; /* currently running uplink config */

/* global uplink connects, and protocol traffic accounters */

struct portaccount_t uplink_connects = {
  .mutex    = PTHREAD_MUTEX_INITIALIZER,
  .refcount = 99,	/* Global static blocks have extra-high initial refcount */
};

static struct timespec resolvconf_mtim = { .tv_sec = 0, .tv_nsec = 0 };
static int resolvconf_reread = 0;

// Work around a glibc bug where resolv.conf is not reread after being changed.
#define RESOLV_CONF_PATH "/etc/resolv.conf"
void consider_res_init(void) {
	struct stat resolvconf_stat;
	if (stat(RESOLV_CONF_PATH, &resolvconf_stat) == -1) {
		hlog(LOG_WARNING, "Failed to stat " RESOLV_CONF_PATH ": %s", strerror(errno));
		return;
	}
	if (resolvconf_mtim.tv_sec == resolvconf_stat.st_mtim.tv_sec
	    && resolvconf_mtim.tv_nsec == resolvconf_stat.st_mtim.tv_nsec) {
	    	return;
	}
	resolvconf_mtim = resolvconf_stat.st_mtim;

	// only log on repeated inits; otherwise would be logged on every startup
	if (resolvconf_reread)
		hlog(LOG_INFO, RESOLV_CONF_PATH " has changed, initializing resolver again");
	resolvconf_reread = 1;

	if (res_init() != 0) {
		hlog(LOG_ERR, "res_init failed, DNS resolver might have problems: %s", strerror(errno));
	}
}

void uplink_client_free(struct uplink_client_t *uc)
{
	if (!uc)
		return;

	*uc->prevp = uc->next;
	if (uc->next)
		uc->next->prevp = uc->prevp;

	hfree(uc);
}

/*
 *	Close uplinking sockets
 */

void close_uplinkers(void)
{
	int rc;
	
	hlog(LOG_DEBUG, "Closing all uplinks");

	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog( LOG_ERR, "close_uplinkers(): could not lock uplink_client_mutex: %s", strerror(rc) );
		return;
	}
	
	for (struct uplink_client_t *uc = uplink_clients; (uc); uc = uc->next) {
		if (uc->client && uc->client->fd >= 0) {
			hlog( LOG_DEBUG, "Closing uplinking socket (fd %d) %s ...", uc->client->fd, uc->client->addr_rem );
			shutdown(uc->client->fd, SHUT_RDWR);
		}
	}
	
	if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog( LOG_ERR, "close_uplinkers(): could not unlock uplink_client_mutex: %s", strerror(rc) );
		return;
	}
	return;
}

/*
 *	Log and handle the closing of an uplink.
 */

void uplink_close(struct client_t *c, int errnum)
{
	int rc;

	hlog(LOG_INFO, "%s: Uplink has been closed: %s", c->addr_rem, aprsc_strerror(errnum));

	if ((rc = pthread_mutex_lock(& uplink_connects.mutex )))
		hlog(LOG_ERR, "uplink_close: could not lock uplink_connects: %s", strerror(rc));
	-- uplink_connects.gauge;
	if ((rc = pthread_mutex_unlock(& uplink_connects.mutex )))
		hlog(LOG_ERR, "uplink_close: could not unlock uplink_connects: %s", strerror(rc));
	
	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "uplink_close(): could not lock uplink_client_mutex: %s", strerror(rc));
		return;
	}

	struct uplink_config_t *l = uplink_config;
	for (; l; l = l->next) {
		if (l->client_ptr == (void *)c) {
			hlog(LOG_DEBUG, "found the link to disconnect");
			l->state = UPLINK_ST_NOT_LINKED;
			l->client_ptr = NULL;
		}
	}

	for (struct uplink_client_t *uc = uplink_clients; (uc); uc = uc->next) {
		if (uc->client == c) {
			uplink_client_free(uc);
			break;
		}
	}

	if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "uplink_close(): could not unlock uplink_client_mutex: %s", strerror(rc));
		return;
	}
	return;
}

/*
 *	uplink_logresp_handler parses the "# logresp" string given by
 *	an upstream server after our "user" command has been sent.
 */

int uplink_logresp_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len)
{
	int argc;
	char *argv[256];
	char *p;
	
	hlog_packet(LOG_INFO, s, len, "%s: Uplink server login response: ", c->addr_rem);
	
	/* parse to arguments */
	/* make it null-terminated for our string processing */
	char *e = s + len;
	*e = 0;
	if ((argc = parse_args_noshell(argv, s)) == 0 || *argv[0] != '#') {
		hlog(LOG_ERR, "%s: Uplink's logresp message is not recognized: no # in beginning (protocol incompatibility)", c->addr_rem);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	if (argc < 6) {
		hlog(LOG_ERR, "%s: Uplink's logresp message does not have enough arguments (protocol incompatibility)", c->addr_rem);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	if (strcmp(argv[1], "logresp") != 0) {
		hlog(LOG_ERR, "%s: Uplink's logresp message does not say 'logresp' (protocol incompatibility)", c->addr_rem);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	if (strcmp(argv[2], serverid) != 0) {
		hlog(LOG_ERR, "%s: Uplink's logresp message does not have my callsign '%s' on it (protocol incompatibility)", c->addr_rem, serverid);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	if (strcmp(argv[3], "verified,") != 0) {
		hlog(LOG_ERR, "%s: Uplink's logresp message does not say I'm verified (wrong passcode in my configuration?)", c->addr_rem);
		client_close(self, c, CLIERR_UPLINK_LOGIN_NOT_VERIFIED);
		return 0;
	}
	
	if (strcmp(argv[4], "server") != 0) {
		hlog(LOG_ERR, "%s: Uplink's logresp message does not contain 'server' (protocol incompatibility)", c->addr_rem);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	p = strchr(argv[5], ',');
	if (p)
		*p = 0;
	
	if (strlen(argv[5]) > CALLSIGNLEN_MAX) {
		hlog(LOG_ERR, "%s: Uplink's server name is too long: '%s'", c->addr_rem, argv[5]);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	if (strcasecmp(argv[5], serverid) == 0) {
		hlog(LOG_ERR, "%s: Uplink's server name is same as ours: '%s'", c->addr_rem, argv[5]);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	/* todo: validate server callsign with the q valid path algorithm */
	
	/* store the remote server's callsign as the "client username" */
	strncpy(c->username, argv[5], sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	
	/* uplink servers are always "validated" */
	c->validated = VALIDATED_WEAK;
	
	/* check the server name against certificate */
#ifdef USE_SSL
	if (c->ssl_con && c->ssl_con->validate) {
		hlog(LOG_DEBUG, "%s/%s: Uplink: Validating TLS server cert subject", c->addr_rem, c->username);
		int ssl_res = ssl_validate_peer_cert_phase2(c);
		
		if (ssl_res != 0) {
			hlog(LOG_WARNING, "%s/%s: TLS server cert validation failed: %s", c->addr_rem, c->username, ssl_strerror(ssl_res));
			client_close(self, c, CLIERR_UPLINK_PEER_CERT_FAIL);
			return 0;
		}
		
		c->validated = VALIDATED_STRONG;
	}
#endif
	
	hlog(LOG_INFO, "%s: Uplink logged in to server %s", c->addr_rem, c->username);
	
	c->handler_line_in = incoming_handler;
	
	/* mark as connected and classify */
	worker_mark_client_connected(self, c);
	
	return 0;
}

/*
 *	uplink_login_handler parses the "# <software> <version" string given by
 *	an upstream server.
 */

int uplink_login_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len)
{
	char buf[1000];
	int rc;
	int argc;
	char *argv[256];

	hlog_packet(LOG_INFO, s, len, "%s: Uplink server software: ", c->addr_rem);
	
#ifdef USE_SSL
	if (c->ssl_con && c->ssl_con->validate) {
		hlog(LOG_DEBUG, "%s/%s: Uplink: Validating TLS server cert against CA", c->addr_rem, c->username);
		int ssl_res = ssl_validate_peer_cert_phase1(c);
		
		if (ssl_res != 0) {
			hlog(LOG_WARNING, "%s/%s: TLS server cert validation failed: %s", c->addr_rem, c->username, ssl_strerror(ssl_res));
			client_close(self, c, CLIERR_UPLINK_PEER_CERT_FAIL);
			return 0;
		}
	}
#endif
	
	/* parse to arguments */
	/* make it null-terminated for our string processing */
	char *e = s + len;
	*e = 0;
	if ((argc = parse_args_noshell(argv, s)) == 0 || *argv[0] != '#') {
		hlog(LOG_ERR, "%s: Uplink's welcome message is not recognized: no # in beginning", c->addr_rem);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return 0;
	}
	
	if (argc >= 3) {
		strncpy(c->app_name, argv[1], sizeof(c->app_name));
		c->app_name[sizeof(c->app_name)-1] = 0;
		strncpy(c->app_version, argv[2], sizeof(c->app_version));
		c->app_version[sizeof(c->app_version)-1] = 0;
	}

	// TODO: The uplink login command here could maybe be improved to send a filter command.
	len = sprintf(buf, "user %s pass %s vers %s\r\n", serverid, passcode, verstr_aprsis);

	hlog(LOG_DEBUG, "%s: my login string: \"%.*s\"", c->addr_rem, len-2, buf);

	rc = c->write(self, c, buf, len);
	if (rc < -2) return rc; // the client was destroyed by client_write, don't touch it

	c->handler_line_in = uplink_logresp_handler;
	c->state   = CSTATE_LOGRESP;
	
	hlog(LOG_INFO, "%s: Connected to server, logging in", c->addr_rem);
	
	return 0;
}

#ifdef USE_SSL
int config_uplink_ssl_setup(struct uplink_config_t *l)
{
	l->ssl = ssl_alloc();
	
	if (ssl_create(l->ssl, (void *)l)) {
		hlog(LOG_ERR, "Uplink: Failed to create TLS context for '%s*'", l->name);
		return -1;
	}
	
	/* optional client cert for server-side validation */
	if (l->certfile && l->keyfile) {
		if (ssl_certificate(l->ssl, l->certfile, l->keyfile)) {
			hlog(LOG_ERR, "Uplink '%s': Failed to load TLS certificatess", l->name);
			ssl_free(l->ssl);
			l->ssl = NULL;
			return -1;
		}
	}
	
	/* optional server cert validation */
	if (l->cafile) {
		if (ssl_ca_certificate(l->ssl, l->cafile, 2)) {
			hlog(LOG_ERR, "Uplink '%s': Failed to load trusted TLS CA certificates", l->name);
			ssl_free(l->ssl);
			l->ssl = NULL;
			return -1;
		}
	}
	
	hlog(LOG_INFO, "Uplink %s: TLS initialized%s%s",
		l->name,
		(l->cafile) ? ", server validated" : "",
		(l->certfile) ? ", client cert loaded" : "");
		
	return 0;
}
#endif


/*
 *	Uplink a single connection
 */

int make_uplink(struct uplink_config_t *l)
{
	int fd, i, addrc, arg;
	struct uplink_client_t *uc = NULL;
	union sockaddr_u sa; /* large enough for also IPv6 address */
	socklen_t addr_len;
	struct addrinfo *ai, *a, *ap[21];
	struct addrinfo req;
	char *addr_s = NULL;
	int port;
	struct sockaddr *srcaddr;
	socklen_t srcaddr_len;

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = AI_ADDRCONFIG;
	ai = NULL;

#ifdef USE_SSL	
	/* SSL requires both a cert and a key, or none at all */
	if ((l->certfile && !l->keyfile) || (l->keyfile && !l->certfile)) {
		hlog(LOG_ERR, "Uplink %s: Only one of tlskey and tlscert defined - both needed for TLS authentication", l->name);
		return -2;
	}
	
	/* todo: allow triggering TLS without client auth */
	if (l->keyfile && l->certfile) {
		if (!l->ssl) {
			if (config_uplink_ssl_setup(l)) {
				hlog(LOG_ERR, "Uplink '%s': TLS setup failed", l->name);
				return -2;
			}
		}
	}
#endif
	
	if (strcasecmp(l->proto, "tcp") == 0) {
		// well, do nothing for now.
	} else if (strcasecmp(l->proto, "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#ifdef USE_SCTP
	} else if (strcasecmp(l->proto, "sctp") == 0) {
		req.ai_socktype = SOCK_STREAM;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "Uplink %s: Unsupported protocol '%s'\n", l->name, l->proto);
		return -2;
	}
	
	port = atoi(l->port);
	if (port < 1 || port > 65535) {
		hlog(LOG_ERR, "Uplink %s: unsupported port number '%s'\n", l->name, l->port);
		return -2;
	}

	l->state = UPLINK_ST_CONNECTING;

	consider_res_init(); // Work around a glibc bug where resolv.conf is not reread on change
	i = getaddrinfo(l->host, l->port, &req, &ai);
	if (i != 0) {
		hlog(LOG_INFO, "Uplink %s: address resolving failure of '%s' '%s': %s", l->name, l->host, l->port, gai_strerror(i));
		l->state = UPLINK_ST_NOT_LINKED;
		return -2;
	}

	/* Count the amount of addresses in response */
	addrc = 0;
	for (a = ai; a && addrc < 20 ; a = a->ai_next, ++addrc) {
		ap[addrc] = a; /* Up to 20 first addresses */
	}
	ap[addrc] = NULL;
	
	if (addrc == 0) {
		hlog(LOG_INFO, "Uplink %s: address resolving of '%s' '%s': returned 0 addresses", l->name, l->host, l->port);
		l->state = UPLINK_ST_NOT_LINKED;
		return -2;
	}
	
	/* Pick random address to start from */
	// coverity[dont_call]  // squelch warning: not security sensitive use of random()
	i = random() % addrc;
	
	/* Then lets try making socket and connection in address order */
	/* TODO: BUG: If the TCP connection succeeds, but the server rejects our
	 * login due to a bad source address (like, IPv4 would be allowed but our
	 * IPv6 address is not in the server's ACL), this currently does not switch
	 * to the next destination address.
	 * Instead it'll wait for the retry timer and then try a random
	 * destination address, and eventually succeed (unless very unlucky).
	 */
	fd = -1;
	while ((a = ap[i])) {
		ap[i] = NULL;
		addr_s = strsockaddr(a->ai_addr, a->ai_addrlen);

		hlog(LOG_INFO, "Uplink %s: Connecting to %s:%s (%s) [addr %d/%d]",
			l->name, l->host, l->port, addr_s, i+1, addrc);
		i++;
		if (i == addrc)
			i = 0;
		
		if ((fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) < 0) {
			hlog(LOG_CRIT, "Uplink %s: socket(): %s\n", l->name, strerror(errno));
			hfree(addr_s);
			continue;
		}
		
		arg = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&arg, sizeof(arg)))
			hlog(LOG_ERR, "Uplink %s: Failed to set SO_REUSEADDR on new socket: %s", l->name, strerror(errno));
		
		/* bind source address */
		srcaddr_len = 0;
		if (a->ai_family == AF_INET && uplink_bind_v4_len != 0) {
			srcaddr = (struct sockaddr *)&uplink_bind_v4;
			srcaddr_len = uplink_bind_v4_len;
		} else if (a->ai_family == AF_INET6 && uplink_bind_v6_len != 0) {
			srcaddr = (struct sockaddr *)&uplink_bind_v6;
			srcaddr_len = uplink_bind_v6_len;
		}
		
		if (srcaddr_len) {
			if (bind(fd, srcaddr, srcaddr_len)) {
				char *s = strsockaddr(srcaddr, srcaddr_len);
				hlog(LOG_ERR, "Uplink %s: Failed to bind source address '%s': %s", l->name, s, strerror(errno));
				hfree(s);
				goto connerr;
			}
		}
		
		/* set non-blocking mode at this point, so that we can make a
		 * non-blocking connect() with a short timeout
		 */
		if (fcntl(fd, F_SETFL, O_NONBLOCK)) {
			hlog(LOG_CRIT, "Uplink %s: Failed to set non-blocking mode on new socket: %s", l->name, strerror(errno));
			goto connerr;
		}
		
		/* Use TCP_NODELAY for APRS-IS sockets. High delays can cause packets getting past
		 * the dupe filters.
		 */
#ifdef TCP_NODELAY
		if (a->ai_protocol == IPPROTO_TCP) {
			int arg = 1;
			if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&arg, sizeof(arg)))
				hlog(LOG_ERR, "Uplink %s: %s: setsockopt(TCP_NODELAY, %d) failed: %s", l->name, addr_s, arg, strerror(errno));
		}
#endif

		if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) {
			hlog(LOG_ERR, "Uplink %s: connect(%s) failed: %s", l->name, addr_s, strerror(errno));
			goto connerr;
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
		hlog(LOG_DEBUG, "Uplink %s: poll after connect returned %d, revents %d", l->name, r, connect_fd.revents);
		
		if (r < 0) {
			hlog(LOG_ERR, "Uplink %s: connect to %s: poll failed: %s", l->name, addr_s, strerror(errno));
			goto connerr;
		}
		
		if (r < 1) {
			hlog(LOG_ERR, "Uplink %s: connect to %s timed out", l->name, addr_s);
			goto connerr;
		}
		
		socklen_t optlen = sizeof(arg);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&arg, &optlen) == -1) {
			hlog(LOG_ERR, "Uplink %s: getsockopt() after connect failed: %s", l->name, strerror(errno));
			goto connerr;
		} else if (arg == 0) {
			/* Successful connect! */
			hlog(LOG_DEBUG, "Uplink %s: successful connect", l->name);
			break;
		}
		
		hlog(LOG_ERR, "Uplink %s: connect to %s failed: %s", l->name, addr_s, strerror(arg));
connerr:
		close(fd);
		fd = -1;
		hfree(addr_s);
		addr_s = NULL;
	}

	freeaddrinfo(ai); /* Not needed anymore.. */

	if (fd < 0) {
		l->state = UPLINK_ST_NOT_LINKED;
		return -3; /* No successfull connection at any address.. */
	}

	struct client_t *c = client_alloc();
	if (!c) {
		hlog(LOG_ERR, "Uplink %s: client_alloc() failed, too many clients", l->name);
		close(fd);
		l->state = UPLINK_ST_NOT_LINKED;
		hfree(addr_s);
		return -3; /* No successfull connection at any address.. */
	}
	
	l->client_ptr = (void *)c;
	c->fd    = fd;
	c->ai_protocol = req.ai_protocol;
	c->state = CSTATE_INIT;
	/* use the default login handler */
	c->handler_line_in = &uplink_login_handler;
	c->flags    = l->client_flags;
	c->keepalive = tick;
	c->last_read = tick;
	c->connect_time = now;
	strncpy(c->username, l->name, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	c->username_len = strlen(c->username);

	/* These peer/sock name calls can not fail -- or the socket closed
	   on us in which case it gets abandoned a bit further below. */

	addr_len = sizeof(sa);
	if (getpeername(fd, (struct sockaddr *)&sa, &addr_len) == -1) {
		hlog(LOG_ERR, "Uplink %s: Failed to get socket peer name: %s", l->name, strerror(errno));
	} else {
		//s = strsockaddr( &sa.sa, addr_len ); /* server side address */
		strncpy(c->addr_rem, addr_s, sizeof(c->addr_rem));
		c->addr_rem[sizeof(c->addr_rem)-1] = 0;
		hfree(addr_s);
		addr_s = NULL;
		c->addr  = sa;

		/* hex format of client's IP address + port */
		char *s = hexsockaddr( &sa.sa, addr_len );
		strncpy(c->addr_hex, s, sizeof(c->addr_hex));
		c->addr_hex[sizeof(c->addr_hex)-1] = 0;
		hfree(s);
	}

	addr_len = sizeof(sa);
	if (getsockname(fd, (struct sockaddr *)&sa, &addr_len) == -1) {
		hlog(LOG_ERR, "Uplink %s: Failed to get local socket address: %s", l->name, strerror(errno));
		c->addr_loc[0] = 0;
	} else {
		char *s = strsockaddr( &sa.sa, addr_len ); /* client side address */
		strncpy(c->addr_loc, s, sizeof(c->addr_loc));
		c->addr_loc[sizeof(c->addr_loc)-1] = 0;
		hfree(s);
	}

	hlog(LOG_INFO, "Uplink %s: %s: Connection established on fd %d using source address %s", l->name, c->addr_rem, c->fd, c->addr_loc);

	if (set_client_sockopt(c) < 0)
		goto err;

	/* insert to the list of currently-connected uplinks */
	int rc;
	if ((rc = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog(LOG_ERR, "make_uplink(): could not lock uplink_client_mutex: %s", strerror(rc));
		goto err;
	}

	uc = hmalloc(sizeof(*uc));
	uc->client = c;
	uc->next = uplink_clients;
	uc->prevp = &uplink_clients;
	if (uc->next)
		uc->next->prevp = &uc->next;
	uplink_clients = uc;

	if ((rc = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog(LOG_CRIT, "make_uplink(): could not unlock uplink_client_mutex: %s", strerror(rc));
		goto err;
	}

	l->state = UPLINK_ST_CONNECTED;
	
	/* set up TLS if necessary */
#ifdef USE_SSL
	if (l->ssl) {
		if (ssl_create_connection(l->ssl, c, 1))
			goto err;
	}
#endif
	
	/* Push it on the first worker, which ever it is..
	 */
	
	if (pass_client_to_worker(worker_threads, c))
		goto err;

	if ((i = pthread_mutex_lock(& uplink_connects.mutex )))
		hlog(LOG_ERR, "make_uplink: could not lock uplink_connects: %s", strerror(i));
	++ uplink_connects.gauge;
	++ uplink_connects.counter;
	++ uplink_connects.refcount;  /* <-- that does not get decremented at any time..  */
	if ((i = pthread_mutex_unlock(& uplink_connects.mutex )))
		hlog(LOG_ERR, "make_uplink: could not unlock uplink_connects: %s", strerror(i));
	
	c->portaccount = & uplink_connects; /* calculate traffic bytes/packets */
	
	return 0;
	
err:
	client_free(c);
	if (uc)
		uplink_client_free(uc);
	l->state = UPLINK_ST_NOT_LINKED;
	if (addr_s)
		hfree(addr_s);
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
	int uplink_error_set = -1;
	
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
	
	//hlog(LOG_INFO, "Uplink thread starting...");
	
	uplink_reconfiguring = 1;
	while (!uplink_shutting_down) {
		if (uplink_reconfiguring || uplink_config_updated) {
			hlog(LOG_INFO, "Uplink thread applying new configuration...");
			__sync_synchronize();
			uplink_reconfiguring = 0;
			close_uplinkers();
			
			free_uplink_config(&uplink_config);
			uplink_config = uplink_config_install;
			uplink_config_install = NULL;
			if (uplink_config)
				uplink_config->prevp = &uplink_config;
			
			uplink_config_updated = 0;
			
			hlog(LOG_INFO, "Uplink thread configured.");
		}
		
		/* speed up shutdown */
		if (uplink_shutting_down || uplink_reconfiguring)
			continue;
		
		if (uplink_config_updated) {
			uplink_reconfiguring = 1;
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
			if (uplink_shutting_down)
				break;
		}
		
		if (avail_uplink && !has_uplink) {
			hlog(LOG_INFO, "Uplink: %d uplinks configured, %d are connected, need to pick new", avail_uplink, has_uplink);
			/* we have regular uplinks but none are connected,
			 * pick the next one and connect */
			next_uplink++;
			if (next_uplink >= avail_uplink)
				next_uplink = 0;
			//hlog(LOG_DEBUG, "Uplink: picked uplink %d as the new candidate", next_uplink);
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
		
		if (avail_uplink && !has_uplink) {
			status_error(3600, "no_uplink");
			uplink_error_set = 1;
		} else {
			if (uplink_error_set == 1) {
				status_error(-1, "no_uplink");
				uplink_error_set = 0;
			}
		}
		
		if (uplink_shutting_down)
			break;

		/* sleep for 4 seconds between successful rounds */
		for (rc = 0; (!uplink_shutting_down) && rc < 4000/200; rc++)
			if (poll(NULL, 0, 200) == -1 && errno != EINTR)
				hlog(LOG_WARNING, "uplink: poll sleep failed: %s", strerror(errno));
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
	
	/* free uplink config - and clean up the uplink_client indexed pointers
	 * which refer to the configs
	 */
	if ((e = pthread_mutex_lock(&uplink_client_mutex))) {
		hlog( LOG_ERR, "uplink_stop(): could not lock uplink_client_mutex: %s", strerror(e) );
		return;
	}
	
	while (uplink_clients)
		uplink_client_free(uplink_clients);
	
	free_uplink_config(&uplink_config);
	
	if ((e = pthread_mutex_unlock(&uplink_client_mutex))) {
		hlog( LOG_ERR, "uplink_stop(): could not unlock uplink_client_mutex: %s", strerror(e) );
		return;
	}
}
