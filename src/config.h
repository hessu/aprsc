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

#ifndef AI_PASSIVE
#include "netdb6.h"
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

extern int lastposition_storetime;
extern int dupefilter_storetime;
extern int heard_list_storetime;
extern int courtesy_list_storetime;
extern int upstream_timeout;
extern int client_timeout;
extern int client_login_timeout;

extern int disallow_unverified;		/* don't allow unverified clients to transmit packets with srccall != login */

extern int verbose;

extern char *serverid;
extern char *passcode;
extern char *myemail;
extern char *myadmin;

extern char *cfgfile;
extern char *pidfile;
extern char *rundir;
extern char *webdir;
extern char *logdir;
extern char *logname;
extern char *chrootdir;
extern char *setuid_s;

#define LISTEN_MAX_FILTERS 10

struct listen_config_t {
	struct listen_config_t *next;
	struct listen_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	const char *name;			/* name of socket */
	const char *host;			/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	int   portnum;
	int   clients_max;
	int   corepeer;
	int   hidden;

	struct addrinfo *ai;
	struct acl_t *acl;
	
	const char *filters[LISTEN_MAX_FILTERS];		/* up to 10 filters, NULL when not defined */

	int client_flags;
};

struct peerip_config_t {
	struct peerip_config_t *next;
	struct peerip_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	const char *name;			/* name of socket */
	const char *host;			/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	int   af;
	int remote_port;
	int local_port;

	struct addrinfo *ai;

	int client_flags;
};

struct uplink_config_t {
	struct uplink_config_t *next;
	struct uplink_config_t **prevp; /* pointer to the *next pointer in the previous node */
	
	const char *name;			/* name of socket */
	const char *proto;
	const char *host;			/* hostname or dotted-quad IP to bind the UDP socket to, default INADDR_ANY */
	const char *port;

	const char *filters[LISTEN_MAX_FILTERS];	/* up to 10 filters, NULL when not defined */

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
extern struct uplink_config_t *uplink_config;

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

extern int read_config(void);
extern void free_config(void);

#endif

