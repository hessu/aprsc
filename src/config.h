/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef CONFIG_H
#define CONFIG_H

#define PROGNAME "aprsc"
#define CRLF "\r\n"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <netdb.h>

#include "ac-hdrs.h"
#include "acl.h"
#include "ssl.h"

#ifndef AI_PASSIVE
#include "netdb6.h"
#endif

/* do we use posix capabilities? */
#ifdef HAVE_PRCTL_H
#ifdef HAVE_CAPABILITY_H
#define USE_POSIX_CAP
#endif
#endif

/* do we use eventfd? No. */
#undef HAVE_EVENTFD_H
#ifdef HAVE_EVENTFD_H
#include <sys/eventfd.h>
#ifdef EFD_NONBLOCK
#ifdef EFD_CLOEXEC
#define USE_EVENTFD
#endif
#endif
#endif

/* do we use clock_gettime to get monotonic time? */
#include <time.h>
#ifdef HAVE_CLOCK_GETTIME
#ifdef CLOCK_MONOTONIC
#define USE_CLOCK_GETTIME
#endif
#endif

/* SCTP? */
#ifdef HAVE_NETINET_SCTP_H
#if defined(SOCK_SEQPACKET) && defined(IPPROTO_SCTP)
#define USE_SCTP
#endif
#endif

extern int fork_a_daemon;	/* fork a daemon */

extern int dump_requests;	/* print requests */
extern int dump_splay;		/* print splay tree information */

extern int workers_configured;	/* number of workers to run */

extern int stats_interval;
extern int expiry_interval;

extern int pbuf_global_expiration;
extern int pbuf_global_dupe_expiration;


extern int obuf_size;
extern int ibuf_size;

extern int new_fileno_limit;
extern int maxclients;

extern int lastposition_storetime;
extern int dupefilter_storetime;
extern int heard_list_storetime;
extern int courtesy_list_storetime;
extern int upstream_timeout;
extern int client_timeout;
extern int client_login_timeout;

extern int disallow_unverified;		/* don't allow unverified clients to transmit packets */
extern int quirks_mode;

extern int verbose;

extern char *serverid;
extern int serverid_len;
extern char *passcode;
extern char *myemail;
extern char *myadmin;
extern char *http_status_options;
extern char *fake_version;

extern char **disallow_srccall_glob;
extern char **disallow_login_glob;

extern char **allow_srccall_glob;
extern char **allow_login_glob;

extern char def_cfgfile[];
extern char *cfgfile;
extern char *pidfile;
extern char *rundir;
extern char *webdir;
extern char def_logname[];
extern char *logname;
extern char *chrootdir;
extern char *setuid_s;

extern int disallow_other_protocol_id;
extern char q_protocol_id;

#define LISTEN_MAX_FILTERS 10

struct listen_config_t {
	struct listen_config_t *next;
	struct listen_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	int   id;			/* id of listener config */
	
	const char *proto;		/* protocol: tcp / udp / sctp */
	const char *name;		/* name of socket */
	const char *host;		/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	int   portnum;
	int   clients_max;
	int   corepeer;			/* special listener for corepeer packets */
	int   hidden;
	
	const char *keyfile;		/* SSL server key file */
	const char *certfile;		/* SSL server certificate file */
	const char *cafile;		/* SSL ca certificate for validating client certs */
	const char *crlfile;		/* SSL certificate revocation file */

	struct addrinfo *ai;
	struct acl_t *acl;
	
	const char *filters[LISTEN_MAX_FILTERS];		/* up to 10 filters, NULL when not defined */
	
	int client_flags;	/* cflags set for clients of this socket */
	
	/* reconfiguration support flags */
	int   changed;		/* configuration has changed */
};

struct peerip_config_t {
	struct peerip_config_t *next;
	struct peerip_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	const char *name;			/* name of socket */
	const char *host;			/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	const char *serverid;			/* expected/configured serverid of remote */
	struct addrinfo *ai;
	
	int   af;
	int remote_port;
	int local_port;


	int client_flags;
};

struct uplink_config_t {
	struct uplink_config_t *next;
	struct uplink_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	const char *name;			/* name of socket */
	const char *proto;
	const char *host;			/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	const char *port;
	
	const char *keyfile;			/* SSL client key file */
	const char *certfile;			/* SSL client certificate file */
	const char *cafile;			/* SSL ca certificate for validating server certs */
	const char *crlfile;			/* SSL certificate revocation file */

#ifdef USE_SSL
	struct ssl_t *ssl;			/* SSL state */
#endif

	int client_flags;
	int state;				/* the state of the uplink */
	void *client_ptr;			/* pointer to the client structure for state matching */
};

#define UPLINK_ST_UNKNOWN	-1
#define UPLINK_ST_NOT_LINKED	0
#define UPLINK_ST_CONNECTING	1
#define UPLINK_ST_CONNECTED	2
#define UPLINK_ST_LINKED	3

extern struct listen_config_t *listen_config;
extern struct peerip_config_t *peerip_config;
extern struct uplink_config_t *uplink_config_install;
extern int uplink_config_updated;
extern int listen_low_ports;

extern struct sockaddr_in uplink_bind_v4;		/* address to bind when connecting out */
extern socklen_t uplink_bind_v4_len;
extern struct sockaddr_in6 uplink_bind_v6;		/* and the same for IPv6 */
extern socklen_t uplink_bind_v6_len;

#define MAX_COREPEERS		16

/* http server config */

struct http_config_t {
	struct http_config_t *next;
	struct http_config_t **prevp;
	
	char *host;			/* name of socket */
	int port;
	
	int upload_port;
	
	struct acl_t *acl;
};

extern struct http_config_t *http_config;

extern char *http_bind;
extern int http_port;
extern char *http_bind_upload;
extern int http_port_upload;

extern int parse_args_noshell(char *argv[],char *cmd);
extern void sanitize_ascii_string(char *s);

extern void free_uplink_config(struct uplink_config_t **lc);
extern struct listen_config_t *find_listen_config_id(struct listen_config_t *l, int id);

extern int read_config(void);
extern void free_config(void);

#endif

