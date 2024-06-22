/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

/*
 *	config.c: configuration parsing, based on Tomi's code
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
                    
#include "config.h"
#include "hmalloc.h"
#include "hlog.h"
#include "cfgfile.h"
#include "worker.h"
#include "filter.h"
#include "parse_qc.h"
#include "tls.h"

char def_cfgfile[] = "aprsc.conf";
char def_webdir[] = "web";

char *cfgfile = def_cfgfile;
char *pidfile;
char *new_rundir;
char *rundir;
//char *new_webdir;
char *webdir = def_webdir;
char *chrootdir = NULL;
char *setuid_s = NULL;

int disallow_other_protocol_id = 1; /* drop packets with other Q protocol identifiers */
#define Q_PROTOCOL_ID_DEFAULT 'A' /* A for APRS-IS */
char q_protocol_id = Q_PROTOCOL_ID_DEFAULT;

char def_logname[] = "aprsc";
char *logname = def_logname;	/* syslog entries use this program name */

char *serverid;
int serverid_len;
char *passcode;
char *myemail;
char *myadmin;
char *fake_version;
char *new_serverid;
char *new_passcode;
char *new_myemail;
char *new_myadmin;
char *new_fake_version;

char **disallow_srccall_glob, **new_disallow_srccall_glob;
char **disallow_login_glob, **new_disallow_login_glob;

int listen_low_ports = 0; /* do we have any < 1024 ports set? need POSIX capabilities? */

struct sockaddr_in uplink_bind_v4;
socklen_t uplink_bind_v4_len = 0;
struct sockaddr_in6 uplink_bind_v6;
socklen_t uplink_bind_v6_len = 0;

struct uplink_config_t *uplink_config_install; /* uplink config waiting to be installed by uplink thread */
int uplink_config_updated = 0;
struct uplink_config_t *new_uplink_config; /* uplink config being generated from config file */

struct peerip_config_t *peerip_config;
struct peerip_config_t *new_peerip_config;

struct http_config_t *http_config = NULL;
struct http_config_t *new_http_config = NULL;
char http_bind_default[] = "0.0.0.0";
char *http_bind = http_bind_default;	/* http address string to listen on */
int http_port = 14501;
char *new_http_bind;
int new_http_port;
char *http_bind_upload = NULL;	/* http address string to listen on */
int http_port_upload = 8080;
char *new_http_bind_upload;
int new_http_port_upload;

char *http_status_options = NULL;
char *new_http_status_options = NULL;

int fork_a_daemon;	/* fork a daemon */

int dump_splay;	/* print splay tree information */

int workers_configured =  2;	/* number of workers to run */

int expiry_interval    = 30;
int stats_interval     = 1 * 60;

int lastposition_storetime = 24*60*60;	/* how long the last position packet of each station is stored */
int dupefilter_storetime   =     30;	/* how long to store information required for dupe filtering */

int heard_list_storetime   =     3*60*60; /* how long to store "client X has heard station Y" information,
                                           * to support text message routing */
int courtesy_list_storetime   =    30*60; /* how long to store "client X has been given MSG from station Y" information,
                                           * to support courtesy position transmission after text message routing */

int pbuf_global_expiration       = 60;  /* 1 minute */ /* 10 sec for load testing */
int pbuf_global_dupe_expiration  = 60;  /* 1 minute */ /* 10 sec for load testing */

int upstream_timeout      = 30;		/* after N seconds of no input from an upstream, disconnect */
int client_timeout        = 48*60*60;	/* after N seconds of no input from a client, disconnect */
int client_login_timeout  = 30;		/* after N seconds of no login command from a client, disconnect */

int disallow_unverified   = 1;		/* disallow packets from unverified clients */
int quirks_mode           = 0;		/* enable quirks mode for all clients by default */

int maxclients = 500;			/* maximum number of clients */

/* These two are not currently used. The fixed defines are in worker.h,
 * OBUF_SIZE and IBUF_SIZE.
 */
int ibuf_size = 8*1024;			/* size of input buffer for clients */
int obuf_size = 8*1024;			/* size of output buffer for clients */

int new_fileno_limit;

int verbose;


/* address:port pairs being listened */
struct listen_config_t *listen_config = NULL;
struct listen_config_t *listen_config_new = NULL;

int do_httpstatus(char *new, int argc, char **argv);
int do_httpupload(char *new, int argc, char **argv);
int do_listen(struct listen_config_t **lq, int argc, char **argv);
int do_interval(int *dest, int argc, char **argv);
int do_peergroup(struct peerip_config_t **lq, int argc, char **argv);
int do_uplink(struct uplink_config_t **lq, int argc, char **argv);
int do_uplinkbind(void *new, int argc, char **argv);
int do_logrotate(int *dest, int argc, char **argv);

/*
 *	Configuration file commands
 */

#define _CFUNC_ (int (*)(void *dest, int argc, char **argv))

static struct cfgcmd cfg_cmds[] = {
	{ "rundir",		_CFUNC_ do_string,	&new_rundir		},
	{ "logrotate",		_CFUNC_ do_logrotate,	&log_rotate_size	},
	{ "serverid",		_CFUNC_ do_string,	&new_serverid		},
	{ "passcode",		_CFUNC_ do_string,	&new_passcode		},
	{ "myemail",		_CFUNC_ do_string,	&new_myemail		},
	{ "myadmin",		_CFUNC_ do_string,	&new_myadmin		},
	{ "workerthreads",	_CFUNC_ do_int,		&workers_configured	},
	{ "statsinterval",	_CFUNC_ do_interval,	&stats_interval		},
	{ "expiryinterval",	_CFUNC_ do_interval,	&expiry_interval	},
	{ "lastpositioncache",	_CFUNC_ do_interval,	&lastposition_storetime	},
	{ "upstreamtimeout",	_CFUNC_ do_interval,	&upstream_timeout	},
	{ "clienttimeout",	_CFUNC_ do_interval,	&client_timeout		},
	{ "logintimeout",	_CFUNC_ do_interval,	&client_login_timeout	},
	{ "filelimit",		_CFUNC_ do_int,		&new_fileno_limit	},
	{ "maxclients",		_CFUNC_ do_int,		&maxclients		},
	{ "ibufsize",		_CFUNC_ do_int,		&ibuf_size		},
	{ "obufsize",		_CFUNC_ do_int,		&obuf_size		},
	{ "httpstatus",		_CFUNC_ do_httpstatus,	&new_http_bind		},
	{ "httpupload",		_CFUNC_ do_httpupload,	&new_http_bind_upload	},
	{ "httpstatusoptions",	_CFUNC_ do_string,	&new_http_status_options	},
	{ "listen",		_CFUNC_ do_listen,	&listen_config_new	},
	{ "uplinkbind",		_CFUNC_ do_uplinkbind,	NULL			},
	{ "uplink",		_CFUNC_ do_uplink,	&new_uplink_config	},
	{ "peergroup",		_CFUNC_ do_peergroup,	&new_peerip_config	},
	{ "q_protocol_id",	_CFUNC_ do_char,	&q_protocol_id	},
	{ "disallow_other_q_protocols",_CFUNC_ do_boolean,	&disallow_other_protocol_id	},
	{ "disallow_unverified",_CFUNC_ do_boolean,	&disallow_unverified	},
	{ "quirks_mode",	_CFUNC_ do_boolean,	&quirks_mode		},
	{ "fake_version",	_CFUNC_ do_string,	&new_fake_version	},
	{ "disallowlogincall",	_CFUNC_ do_string_array,	&new_disallow_login_glob	},
	{ "disallowsourcecall",	_CFUNC_ do_string_array,	&new_disallow_srccall_glob	},
	{ NULL,			NULL,			NULL			}
};

/*
 *	Parse a command line to argv, not honoring quotes or such
 */
 
int parse_args_noshell(char *argv[],char *cmd)
{
	int ct = 0;
	
	while (ct < 255)
	{
		while (*cmd && isspace((int)*cmd))
			cmd++;
		if (*cmd == 0)
			break;
		argv[ct++] = cmd;
		while (*cmd && !isspace((int)*cmd))
			cmd++;
		if (*cmd)
			*cmd++ = 0;
	}
	argv[ct] = NULL;
	return ct;
}

/*
 *	Sanitize an user-entered ASCII string to not contain control chars
 */

void sanitize_ascii_string(char *s)
{
	unsigned char *p;
	
	for (p = (unsigned char *)s; *p; p++) {
		if (iscntrl(*p) || !(isascii(*p)))
			*p = '_';
	}
}


/*
 *	Free a listen config tree
 */

void free_listen_config(struct listen_config_t **lc)
{
	struct listen_config_t *this;
	int i;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		hfree((void*)this->name);
		hfree((void*)this->host);
		hfree((void*)this->proto);
		for (i = 0; i < (sizeof(this->filters)/sizeof(this->filters[0])); ++i)
			if (this->filters[i])
				hfree((void*)this->filters[i]);
		freeaddrinfo(this->ai);
		if (this->acl)
			acl_free(this->acl);
		if (this->keyfile)
			hfree((void *)this->keyfile);
		if (this->certfile)
			hfree((void *)this->certfile);
		if (this->cafile)
			hfree((void *)this->cafile);
		if (this->crlfile)
			hfree((void *)this->crlfile);
		hfree(this);
	}
}

struct listen_config_t *find_listen_config_id(struct listen_config_t *l, int id)
{
	while (l) {
		if (l->id == id)
			return l;
		l = l->next;
	}
	
	return NULL;
}

static struct listen_config_t *find_listen_config(struct listen_config_t *l,
	const char *proto, const char *host, int portnum)
{
	while (l) {
		if (l->portnum == portnum
			&& strcmp(l->host, host) == 0
			&& strcmp(l->proto, proto) == 0)
			return l;
			
		l = l->next;
	}
	
	return NULL;
}


/*
 *	Free a peer-ip config tree
 */

void free_peerip_config(struct peerip_config_t **lc)
{
	struct peerip_config_t *this;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		hfree((void*)this->name);
		hfree((void*)this->host);
		hfree((void*)this->serverid);
		freeaddrinfo(this->ai);
		hfree(this);
	}
}

/*
 *	Free a http config tree
 */

void free_http_config(struct http_config_t **lc)
{
	struct http_config_t *this;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		hfree((void*)this->host);
		hfree(this);
	}
}

/*
 *	Free a uplink config tree
 */

void free_uplink_config(struct uplink_config_t **lc)
{
	struct uplink_config_t *this;

	while (*lc) {
		this = *lc;
		*lc = this->next;
		this->next = (void *)1; /* poison the list, in case of referencing freed entries */
		this->prevp = (void *)1;
		hfree((void*)this->name);
		hfree((void*)this->proto);
		hfree((void*)this->host);
		hfree((void*)this->port);
		hfree((void*)this->keyfile);
		hfree((void*)this->certfile);
		hfree((void*)this->cafile);
		hfree((void*)this->crlfile);
#ifdef USE_SSL
		if (this->ssl)
			ssl_free(this->ssl);
#endif		
		hfree(this);
	}
}

/*
 *	Match two addrinfo chains, return 1 if there are matching
 *	addresses in the chains
 */
 
int ai_comp(struct addrinfo *a, struct addrinfo *b)
{
	struct addrinfo *ap, *bp;
	union sockaddr_u *au, *bu;
	
	for (ap = a; ap; ap = ap->ai_next) {
		for (bp = b; bp; bp = bp->ai_next) {
			if (ap->ai_family != bp->ai_family)
				continue;
			if (ap->ai_addrlen != bp->ai_addrlen)
				continue;
				
			au = (union sockaddr_u *)ap->ai_addr;
			bu = (union sockaddr_u *)bp->ai_addr;
			
			if (ap->ai_family == AF_INET) {
				if (memcmp(&au->si.sin_addr, &bu->si.sin_addr, sizeof(au->si.sin_addr)) != 0)
					continue;
				if (au->si.sin_port != bu->si.sin_port)
					continue;
					
				return 1; // Oops, there is a match
			}
			
			if (ap->ai_family == AF_INET6) {
				if (memcmp(&au->si6.sin6_addr, &bu->si6.sin6_addr, sizeof(au->si6.sin6_addr)) != 0)
					continue;
				if (au->si6.sin6_port != bu->si6.sin6_port)
					continue;
					
				return 1; // Oops, there is a match
			}
		}
	}
	
	return 0;
}

/*
 *	parse an interval specification
 */
 
time_t parse_interval(char *origs)
{
	time_t t = 0;
	int i;
	char *s, *np, *p, c;
	
	np = p = s = hstrdup(origs);
	
	while (*p) {
		if (!isdigit((int)*p)) {
			c = tolower(*p);
			*p = '\0';
			i = atoi(np);
			if (c == 's')
				t += i;
			else if (c == 'm')
				t += 60 * i;
			else if (c == 'h')
				t += 60 * 60 * i;
			else if (c == 'd')
				t += 24 * 60 * 60 * i;
			np = p + 1;
		}
		p++;
	}
	
	if (*np)
		t += atoi(np);
		
	hfree(s);
	return t;
}


/*
 *	Parse an interval configuration entry
 */

int do_interval(int *dest, int argc, char **argv)
{
	if (argc < 2)
		return -1;
		
	*dest = parse_interval(argv[1]);
	return 0;
}

/*
 *	Parse a peer definition directive
 *
 *	"keyword" <token?> [udp|sctp] <localhost>:<localport> <remotehost1>:<remoteport> <remote2> ...
 *
 */


int parse_hostport(char *s, char **host_s, char **port_s)
{
	char *colon;
	char *bracket;
	
	colon = strrchr(s, ':');
	if (colon == NULL)
		return -1;
	
	*colon = 0;
	
	*host_s = s;
	*port_s = colon+1;
	
	if (**host_s == '[') {
		bracket = strrchr(*host_s, ']');
		if (!bracket)
			return -1;
			
		*bracket = 0;
		*host_s = *host_s + 1;
	}
	
	return 0;
}

int do_peergroup(struct peerip_config_t **lq, int argc, char **argv)
{
	int localport, port, i, d;
	struct peerip_config_t *pe;
	struct listen_config_t *li;
	struct addrinfo req, *my_ai, *ai, *a;
	char *peerid = NULL;
	char *fullhost, *host_s, *port_s;
	int af;
	
	ai = my_ai = a = NULL;

	if (argc < 4)
		return -1;
	
	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	my_ai = NULL;

	// Only UDP and SCTP are acceptable for peergroups
	if (strcasecmp(argv[2], "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#ifdef USE_SCTP
	} else if (strcasecmp(argv[2], "sctp") == 0) {
		req.ai_socktype = SOCK_STREAM;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "PeerGroup: Unsupported protocol '%s'", argv[2]);
		return -2;
	}
	
	fullhost = hstrdup(argv[3]);
	
	if (parse_hostport(argv[3], &host_s, &port_s)) {
		hlog(LOG_ERR, "PeerGroup: Invalid local host:port specification '%s'", fullhost);
		hfree(fullhost);
		return -2;
	}
	
	localport = atoi(port_s);
	i = getaddrinfo(host_s, port_s, &req, &my_ai);
	if (i != 0) {
		hlog(LOG_ERR, "PeerGroup: address parsing or hostname lookup failure for %s: %s", fullhost, gai_strerror(i));
		hfree(fullhost);
		return -2;
	}
	
	d = 0;
	for (a = my_ai; (a); a = a->ai_next, ++d);
	if (d != 1) {
		hlog(LOG_ERR, "PeerGroup: address parsing for local address %s returned %d addresses - can only have one", fullhost, d);
		hfree(fullhost);
		freeaddrinfo(my_ai);
		return -2;
	}
	
	af = my_ai->ai_family;
	
	//hlog(LOG_DEBUG, "PeerGroup: configuring with local address %s (local port %d)", fullhost, localport);
	
	/* Configure a listener */
	li = hmalloc(sizeof(*li));
	memset(li, 0, sizeof(*li));
	// coverity[dont_call]  // squelch warning: not security sensitive use of random(): local id
	li->id = random();
	li->corepeer = 1;
	li->name = hstrdup(argv[1]);
	li->host = fullhost;
	li->proto = hstrdup("udp");
	li->portnum      = localport;
	li->client_flags = 0;
	li->clients_max  = 1;
	li->ai = my_ai;
	li->acl = NULL;
	li->next = NULL;
	li->prevp = NULL;
	
	/* there are no filters between peers */
	for (i = 0; i < LISTEN_MAX_FILTERS; i++)
		li->filters[i] = NULL;
	
	/* put in the list */
	li->next = listen_config_new;
	if (li->next)
		li->next->prevp = &li->next;
	listen_config_new = li;
	
	fullhost = NULL;
	
	// TODO: when returning, should free the whole li tree
	for (i = 4; i < argc; i++) {
		//hlog(LOG_DEBUG, "PeerGroup: configuring peer %s", argv[i]);
		
		peerid = hstrdup(argv[i]);
		i++;
		
		if (i >= argc) {
			hlog(LOG_ERR, "PeerGroup: No host:port specified for peer ServerID '%s'", peerid);
			goto err;
		}
		
		/* Parse address */
		fullhost = hstrdup(argv[i]);
		if (parse_hostport(argv[i], &host_s, &port_s)) {
			hlog(LOG_ERR, "PeerGroup: Invalid remote host:port specification '%s'", fullhost);
			goto err;
		}
		
		port = atoi(port_s);
		if (port < 1 || port > 65535) {
			hlog(LOG_ERR, "PeerGroup: Invalid port number '%s' for remote address '%s'", port_s, fullhost);
			goto err;
		}
		
		ai = NULL;
		d = getaddrinfo(host_s, port_s, &req, &ai);
		if (d != 0) {
			hlog(LOG_ERR, "PeerGroup: address parsing or hostname lookup failure for %s: %s", fullhost, gai_strerror(d));
			goto err;
		}
		
		/* we can only take one address per peer at this point, SCTP multihoming ignored */
		d = 0;
		for (a = ai; (a); a = a->ai_next, ++d);
		if (d != 1) {
			hlog(LOG_ERR, "PeerGroup: address parsing for remote %s returned %d addresses - can only have one", fullhost, d);
			goto err;
		}
		
		if (ai->ai_family != af) {
			hlog(LOG_ERR, "PeerGroup: remote address %s has different address family than the local address - mixing IPv4 and IPv6, are we?", fullhost);
			goto err;
		}
		
		/* Check that the address is not mine (loop!), and that we don't have
		 * it configured already (dupes!). My address is ignored quietly, allowing symmetric
		 * peer configs on all nodes.
		 */
		
		if (ai_comp(ai, my_ai)) {
			//hlog(LOG_DEBUG, "PeerGroup: Ignoring %s - it's me, my local address", fullhost);
			hfree(fullhost);
			fullhost = NULL;
			hfree(peerid);
			freeaddrinfo(ai);
			ai = NULL;
			continue;
		}
		
		for (pe = *lq; (pe); pe = pe->next) {
			if (ai_comp(ai, pe->ai)) {
				hlog(LOG_ERR, "PeerGroup: remote address %s is configured as a peer twice, would cause duplicate transmission!", fullhost);
				goto err;
			}
		}
		
		hfree(fullhost);
		fullhost = NULL;
		
		/* Ok, enough checks. Go for it! */
		pe = hmalloc(sizeof(*pe));
		memset(pe, 0, sizeof(*pe));
		pe->name = hstrdup(host_s);
		pe->host = hstrdup(host_s);
		pe->serverid = peerid;
		peerid = NULL;
		pe->af = af;
		pe->local_port = localport;
		pe->remote_port = port;
		pe->client_flags = 0; // ???
		pe->ai = ai;
		
		/* put in the list */
		pe->next = *lq;
		if (pe->next)
			pe->next->prevp = &pe->next;
		*lq = pe;
	}
	
	return 0;
err:
	if (fullhost)
		hfree(fullhost);
	if (peerid)
		hfree(peerid);
	if (ai)
		freeaddrinfo(ai);
		
	return -2;
}

/*
 *	Uplink tls string option handler
 */

int config_uplink_tls(char **argv, int argc, int *i, const char *key, char **dst)
{
#ifdef USE_SSL
	(*i)++;
	if (*i >= argc) {
		hlog(LOG_ERR, "Uplink: '%s' argument is missing the file parameter for '%s'", key, argv[1]);
		return -2;
	}
	
	if (*dst) {
		hlog(LOG_ERR, "Uplink: second '%s' not allowed for '%s'", key, argv[1]);
		return -2;
	}
	
	*dst = hstrdup(argv[*i]);
	return 0;
#else
	hlog(LOG_ERR, "Uplink: '%s' not available for '%s' - server not built with OpenSSL", key, argv[1]);
	return -2;
#endif
}

/*
 *	Parse a uplink definition directive
 *
 *	uplink <label> <token> {tcp|udp|sctp} <hostname> <portnum> [<filter> [..<more_filters>]]
 *
 */

int do_uplink(struct uplink_config_t **lq, int argc, char **argv)
{
	struct uplink_config_t *l;
	int port;
	int clflags = CLFLAGS_UPLINKPORT;
	int i;

	if (argc < 5)
		return -1;

	/* argv[1] is  name label  for this uplink */

	if (strcasecmp(argv[2], "ro")==0) {
		clflags |= CLFLAGS_PORT_RO;
	} else if (strcasecmp(argv[2], "multiro")==0) {
		clflags |= CLFLAGS_PORT_RO|CLFLAGS_UPLINKMULTI;
	} else if (strcasecmp(argv[2], "full") == 0) {
		/* regular */
	} else {
		hlog(LOG_ERR, "Uplink: Unsupported uplink type '%s'", argv[2]);
		return -2;
	}

	if (strcasecmp(argv[3], "tcp") == 0) {
		// well, do nothing for now.
	} else if (strcasecmp(argv[3], "udp") == 0) {
#ifdef USE_SCTP
	} else if (strcasecmp(argv[3], "sctp") == 0) {
#endif
	} else {
		hlog(LOG_ERR, "Uplink: Unsupported protocol '%s'", argv[3]);
		return -2;
	}
	
	port = atoi(argv[5]);
	if (port < 1 || port > 65535) {
		hlog(LOG_ERR, "Uplink: Invalid port number '%s'", argv[5]);
		return -2;
	}

	/* For uplinks, we don't really wish to do a DNS lookup at this point
	 * - we do it when connecting, the address might have changed at that
	 * point. Also, the lookup might slow up the startup considerably if
	 * servers time out. And if the network comes up later than aprsc
	 * after a power failure, we must not ignore the uplink configs now.
	 */

	l = hmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->name  = hstrdup(argv[1]);
	l->proto = hstrdup(argv[3]);
	l->host  = hstrdup(argv[4]);
	l->port  = hstrdup(argv[5]);
	l->client_flags = clflags;
	l->state = UPLINK_ST_UNKNOWN;
	
	for (i = 6; i < argc; i++) {
		if (strcasecmp(argv[i], "tlskey") == 0 || strcasecmp(argv[i], "sslkey") == 0) {
			if (config_uplink_tls(argv, argc, &i, "tlskey", (char **)&l->keyfile)) {
				free_uplink_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "tlscert") == 0 || strcasecmp(argv[i], "sslcert") == 0) {
			if (config_uplink_tls(argv, argc, &i, "tlscert", (char **)&l->certfile)) {
				free_uplink_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "tlsca") == 0 || strcasecmp(argv[i], "sslca") == 0) {
			if (config_uplink_tls(argv, argc, &i, "tlsca", (char **)&l->cafile)) {
				free_uplink_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "tlscrl") == 0 || strcasecmp(argv[i], "sslcrl") == 0) {
			if (config_uplink_tls(argv, argc, &i, "tlscrl", (char **)&l->crlfile)) {
				free_uplink_config(&l);
				return -2;
			}
		} else {
			hlog(LOG_ERR, "Uplink %s: Invalid parameter '%s'", argv[1], argv[i]);
			free_uplink_config(&l);
			return -2;
		}
	}

	/* put in the end of the list */
	while (*lq)
		lq = &(*lq)->next;
		
	l->prevp = lq;
	l->next = NULL;
	*lq = l;
	
	return 0;
}

/*
 *	Uplink source address binding configuration
 */

int do_uplinkbind(void *new, int argc, char **argv)
{
	struct addrinfo req, *ai, *a;
	int i, d;

	if (argc < 2)
		return -1;
	
	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	
	for (i = 1; i < argc; i++) {
		hlog(LOG_DEBUG, "UplinkBind: looking up %s", argv[i]);
		
		ai = NULL;
		d = getaddrinfo(argv[i], "0", &req, &ai);
		if (d != 0 || !(ai)) {
			hlog(LOG_ERR, "UplinkBind: address parsing or hostname lookup failure for %s: %s", argv[i], gai_strerror(d));
			return -2;
		}
		
		/* we can only take one address per peer at this point, SCTP multihoming ignored */
		d = 0;
		for (a = ai; (a); a = a->ai_next, ++d);
		if (d != 1) {
			hlog(LOG_ERR, "UplinkBind: address parsing for %s returned %d addresses - can only have one", argv[i], d);
			freeaddrinfo(ai);
			return -2;
		}
		
		if (ai->ai_family == AF_INET) {
			memcpy(&uplink_bind_v4, ai->ai_addr, ai->ai_addrlen);
			uplink_bind_v4_len = ai->ai_addrlen;
		} else if (ai->ai_family == AF_INET6) {
			memcpy(&uplink_bind_v6, ai->ai_addr, ai->ai_addrlen);
			uplink_bind_v6_len = ai->ai_addrlen;
		} else {
			hlog(LOG_ERR, "UplinkBind: address %s has unknown address family %d", argv[i], ai->ai_family);
			freeaddrinfo(ai);
			return -2;
		}
		
		freeaddrinfo(ai);
	}
	
	return 0;
}


/*
 *	Parse a Listen directive
 *
 *	listen <label> <token> [tcp|udp|sctp] <hostname> <portnum> [<filter> [..<more_filters>]]
 *
 */

int config_parse_listen_filter(struct listen_config_t *l, char *filt_string, char *portname)
{
	int argc;
	char *argv[256];
	int i;
	
	argc = parse_args_noshell(argv, filt_string);
	if (argc == 0) {
		hlog(LOG_ERR, "Listen: Bad filter definition for '%s': '%s' - no filter arguments found",
			portname, filt_string);
		return -1;
	}
	
	if (argc > LISTEN_MAX_FILTERS) {
		hlog(LOG_ERR, "Listen: Bad filter definition for '%s': '%s' - too many (%d) filter arguments found, max %d",
			portname, filt_string, argc, LISTEN_MAX_FILTERS);
		return -1;
	}
	
	for (i = 0; i < argc && i < LISTEN_MAX_FILTERS; i++) {
		if (filter_parse(NULL, argv[i], 0) < 0) {
			hlog(LOG_ERR, "Listen: Bad filter definition for '%s': '%s' - filter parsing failed",
				portname, argv[i]);
			return -1;
		}
		l->filters[i] = hstrdup(argv[i]);
	}
	
	return 0;
}

int config_listen_tls(char **argv, int argc, int *i, const char *key, char **dst)
{
#ifdef USE_SSL
	(*i)++;
	if (*i >= argc) {
		hlog(LOG_ERR, "Listen: '%s' argument is missing the file parameter for '%s'", key, argv[1]);
		return -2;
	}
	
	if (*dst) {
		hlog(LOG_ERR, "Listen: second '%s' not allowed for '%s'", key, argv[1]);
		return -2;
	}
	
	*dst = hstrdup(argv[*i]);
	return 0;
#else
	hlog(LOG_ERR, "Listen: '%s' not available for '%s' - server not built with OpenSSL", key, argv[1]);
	return -2;
#endif
}

int do_listen(struct listen_config_t **lq, int argc, char **argv)
{
	int i, port;
	struct listen_config_t *l;
	struct addrinfo req, *ai;
	/* default parameters for a listener */
	int clflags = CLFLAGS_INPORT;
	int clients_max = 200;
	char *proto;

	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;

	if (argc < 6)
		return -1;

	if (strcasecmp(argv[2], "userfilter") == 0) {
	  clflags |= CLFLAGS_USERFILTEROK;
	  clflags |= CLFLAGS_IGATE;
	} else if (strcasecmp(argv[2], "igate") == 0) {
	  clflags |= CLFLAGS_USERFILTEROK;
	  clflags |= CLFLAGS_IGATE;
	} else if (strcasecmp(argv[2], "fullfeed") == 0) {
	  clflags |= CLFLAGS_FULLFEED;
	} else if (strcasecmp(argv[2], "is2") == 0) {
	  clflags |= CLFLAGS_IGATE;
	  clflags |= CLFLAGS_IS2;
	} else if (strcasecmp(argv[2], "dupefeed") == 0) {
	  clflags |= CLFLAGS_DUPEFEED;
	} else if (strcasecmp(argv[2], "clientonly") == 0) {
	  clflags |= CLFLAGS_CLIENTONLY;
	  clflags |= CLFLAGS_USERFILTEROK;
	  clflags |= CLFLAGS_IGATE;
	} else if (strcasecmp(argv[2], "udpsubmit") == 0) {
	  clflags |= CLFLAGS_CLIENTONLY;
	  clflags |= CLFLAGS_UDPSUBMIT;
	  clients_max = 1;
	} else {
	  hlog(LOG_ERR, "Listen: unknown port type: %s", argv[2]);
	}
	
	proto = argv[3];
	if (strcasecmp(proto, "tcp") == 0) {
		/* well, do nothing for now. */
	} else if (strcasecmp(proto, "udp") == 0) {
		req.ai_socktype = SOCK_DGRAM;
		req.ai_protocol = IPPROTO_UDP;
#ifdef USE_SCTP
	} else if (strcasecmp(proto, "sctp") == 0) {
		req.ai_socktype = SOCK_STREAM;
		req.ai_protocol = IPPROTO_SCTP;
#endif
	} else {
		hlog(LOG_ERR, "Listen: Unsupported protocol '%s'\n", proto);
		return -2;
	}
	
	if ((clflags & CLFLAGS_UDPSUBMIT) && req.ai_protocol != IPPROTO_UDP) {
		hlog(LOG_ERR, "Listen: Invalid protocol '%s' for udpsubmit port - only UDP is supported\n", proto);
		return -2;
	}
	
	port = atoi(argv[5]);
	if (port < 1 || port > 65535) {
		hlog(LOG_ERR, "Listen: Invalid port number '%s'\n", argv[5]);
		return -2;
	}

	i = getaddrinfo(argv[4], argv[5], &req, &ai);
	if (i != 0) {
		hlog(LOG_ERR, "Listen: address parse failure of '%s' '%s': %s", argv[4], argv[5], gai_strerror(i));
		return -2;
	}
	
	l = hmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->name = hstrdup(argv[1]);
	l->host = hstrdup(argv[4]);
	l->proto = hstrdup(proto);
	l->portnum      = port;
	l->client_flags = clflags;
	l->clients_max  = clients_max;
	l->ai = ai;
	l->acl = NULL;
	l->next = NULL;
	l->prevp = NULL;
	
	/* by default, no filters */
	for (i = 0; i < LISTEN_MAX_FILTERS; i++)
		l->filters[i] = NULL;
	
	/* parse rest of arguments */
	i = 6;
	while (i < argc) {
		if (strcasecmp(argv[i], "filter") == 0) {
			/* set a filter for the clients */
			i++;
			if (i >= argc) {
				hlog(LOG_ERR, "Listen: 'filter' argument is missing the filter parameter for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			
			if (clflags & (CLFLAGS_FULLFEED|CLFLAGS_UDPSUBMIT|CLFLAGS_DUPEFEED)) {
				hlog(LOG_ERR, "Listen: '%s': 'filter' argument is not valid for port type of '%s'", argv[1], argv[2]);
				free_listen_config(&l);
				return -2;
			}
			
			if (config_parse_listen_filter(l, argv[i], argv[1])) {
				free_listen_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "maxclients") == 0) {
			/* Limit amount of clients */
			i++;
			if (i >= argc) {
				hlog(LOG_ERR, "Listen: 'maxclients' argument is missing the numeric max clients limit for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			l->clients_max = atoi(argv[i]);
		} else if (strcasecmp(argv[i], "acl") == 0) {
			/* Access list */
			i++;
			if (i >= argc) {
				hlog(LOG_ERR, "Listen: 'acl' argument is missing the acl parameter for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			
			if (l->acl) {
				hlog(LOG_ERR, "Listen: second 'acl' not allowed for '%s'", argv[1]);
				free_listen_config(&l);
				return -2;
			}
			
			l->acl = acl_load(argv[i]);
			if (!l->acl) {
				free_listen_config(&l);
				return -2;
			}

			
		} else if (strcasecmp(argv[i], "tlskey") == 0 || strcasecmp(argv[i], "sslkey") == 0) {
			if (config_listen_tls(argv, argc, &i, "tlskey", (char **)&l->keyfile)) {
				free_listen_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "tlscert") == 0 || strcasecmp(argv[i], "sslcert") == 0) {
			if (config_listen_tls(argv, argc, &i, "tlscert", (char **)&l->certfile)) {
				free_listen_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "tlsca") == 0 || strcasecmp(argv[i], "sslca") == 0) {
			if (config_listen_tls(argv, argc, &i, "tlsca", (char **)&l->cafile)) {
				free_listen_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "tlscrl") == 0 || strcasecmp(argv[i], "sslcrl") == 0) {
			if (config_listen_tls(argv, argc, &i, "tlscrl", (char **)&l->crlfile)) {
				free_listen_config(&l);
				return -2;
			}
		} else if (strcasecmp(argv[i], "hidden") == 0) {
			/* Hide the listener from status view */
			l->hidden = 1;
		} else {
			hlog(LOG_ERR, "Listen: Unknown argument '%s' for '%s'", argv[i], argv[1]);
			free_listen_config(&l);
			return -2;
		}
		i++;
	}
	
	/* SSL requires both a cert and a key */
	if ((l->certfile && !l->keyfile) || (l->keyfile && !l->certfile)) {
		hlog(LOG_ERR, "Listen: Only one of tlskey and tlscert defined for '%s' - both needed for TLS", argv[1]);
		free_listen_config(&l);
		return -2;
	}
	
	/* dupefeed port is always hidden */
	if (clflags & CLFLAGS_DUPEFEED)
		l->hidden = 1;
	
	if (clflags & CLFLAGS_UDPSUBMIT)
		l->clients_max = 1;
	
	/* if low ports are configured, make a note of that, so that
	 * POSIX capability to bind low ports can be reserved
	 * at startup.
	 */
	if (port < 1024)
		listen_low_ports = 1;
	
	/* find existing config for same proto-host-port combination */
	struct listen_config_t *old_l;
	old_l = find_listen_config(listen_config, l->proto, l->host, l->portnum);
	if (old_l) {
		/* this is an old config... see if it changed in a way which
		 * would require listener reconfiguration
		 */
		l->id = old_l->id;
	} else {
		/* new config, assign new id */
		// coverity[dont_call]  // squelch warning: not security sensitive use of random(): local id
		l->id = random();
	}
	
	/* put in the list */
	l->next = *lq;
	if (l->next)
		l->next->prevp = &l->next;
	*lq = l;
	
	return 0;
}

int do_http_listener(char *what, int upload_type, int argc, char **argv)
{
	struct http_config_t *l;
	
	if (argc != 3) {
		hlog(LOG_ERR, "%s: Invalid number of arguments", what);
		return -1;
	}
	
	l = hmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->host = hstrdup(argv[1]);
	l->port = atoi(argv[2]);
	l->upload_port = upload_type;
	
	l->next = new_http_config;
	if (new_http_config)
		new_http_config->prevp = &l->next;
	new_http_config = l;
	l->prevp = &new_http_config;
	
	return 0;
}

int do_httpstatus(char *new, int argc, char **argv)
{
	return do_http_listener("HTTPStatus", 0, argc, argv);
}

int do_httpupload(char *new, int argc, char **argv)
{
	return do_http_listener("HTTPUpload", 1, argc, argv);
}

/*
 *	Log rotation config
 */
 
int do_logrotate(int *dest, int argc, char **argv)
{
	int i;
	
	if (argc != 3) {
		hlog(LOG_ERR, "LogRotate: Invalid number of arguments");
		return -1;
	}
	
	i = atoi(argv[1]);
	if (i < 1) {
		hlog(LOG_ERR, "LogRotate: Invalid megabytes value: %s", argv[1]);
		return -1;
	}
	
	log_rotate_size = i * 1024 * 1024;
	
	i = atoi(argv[2]);
	if (i < 1) {
		hlog(LOG_ERR, "LogRotate: Invalid file count: %s", argv[2]);
		log_rotate_size = 0;
		return -1;
	}
	
	log_rotate_num = i;
	
	hlog(LOG_DEBUG, "LogRotate: Enabled at %d megabytes, %d files",
		log_rotate_size/1024/1024, log_rotate_num);
	
	return 0;
}

/*
 *	upcase
 */
 
char *strupr(char *s)
{
	char *p;
	
	for (p = s; (*p); p++)
		*p = toupper(*p);
		
	return s;
}

/*
 *	test if a path points to a directory
 */

static int test_directory(const char *path)
{
	struct stat sb;
	
	if (stat(path, &sb) != 0) {
		hlog(LOG_ERR, "stat(%s) failed: %s", path, strerror(errno));
		return -1;
	}
	
	if (!S_ISDIR(sb.st_mode)) {
		hlog(LOG_ERR, "%s: Not a directory", path);
		return -1;
	}
	
	return 0;
}

/*
 *	Read configuration files, should add checks for this program's
 *	specific needs and obvious misconfigurations!
 */

int read_config(void)
{
	int failed = 0;
	char *s;
	
	if (read_cfgfile(cfgfile, cfg_cmds))
		return -1;
	
	/* these parameters will only be used when reading the configuration
	 * for the first time.
	 */
	if (!rundir) {
		/* initial setup */
		if (new_rundir) {
			rundir = new_rundir;
			new_rundir = NULL;
		} else {
			hlog(LOG_CRIT, "Config: rundir not defined.");
			failed = 1;
		}
		
		if ((rundir) && test_directory(rundir)) {
			hlog(LOG_CRIT, "Config: rundir %s is not a directory.", rundir);
			failed = 1;
		}
	} else {
		/* reconfig */
		if (new_rundir) {
			hfree(new_rundir);
			new_rundir = NULL;
		}
	}
	
	if (!log_dir) {
		hlog(LOG_CRIT, "Config: logdir not defined.");
		failed = 1;
	}
	
	/* serverid is only applied when running for the first time. */
	if (serverid) {
		if (new_serverid && strcasecmp(new_serverid, serverid) != 0)
			hlog(LOG_ERR, "Config: Not changing serverid while running.");
		hfree(new_serverid);
		new_serverid = NULL;
	} else {
		if (!new_serverid) {
			hlog(LOG_CRIT, "Config: serverid is not defined.");
			failed = 1;
		} else if (check_invalid_q_callsign(new_serverid, strlen(new_serverid)) != 0
		   || strlen(new_serverid) < 3 || strlen(new_serverid) > CALLSIGNLEN_MAX) {
			hlog(LOG_CRIT, "Config: serverid '%s' is not valid.", new_serverid);
			failed = 1;
		} else {
			strupr(new_serverid);
			serverid = new_serverid;
			serverid_len = strlen(serverid);
			new_serverid = NULL;
		}
	}
	
	if (new_passcode) {
		if (passcode)
			hfree(passcode);
		passcode = new_passcode;
		new_passcode = NULL;
	} else {
		hlog(LOG_ERR, "Config: passcode is not defined.");
		failed = 1;
	}
	
	if (new_myadmin) {
		if (myadmin)
			hfree(myadmin);
		myadmin = new_myadmin;
		new_myadmin = NULL;
	} else {
		hlog(LOG_ERR, "Config: myadmin is not defined.");
		failed = 1;
	}
	
	if (new_myemail) {
		if (myemail)
			hfree(myemail);
		myemail = new_myemail;
		new_myemail = NULL;
	} else {
		hlog(LOG_ERR, "Config: myemail is not defined.");
		failed = 1;
	}
	
	if (new_http_bind) {
		if (http_bind && http_bind != http_bind_default)
			hfree(http_bind);
		http_bind = new_http_bind;
		new_http_bind = NULL;
		http_port = new_http_port;
	}
	
	if (new_http_bind_upload) {
		if (http_bind_upload)
			hfree(http_bind_upload);
		http_bind_upload = new_http_bind_upload;
		new_http_bind_upload = NULL;
		http_port_upload = new_http_port_upload;
	}
	
	if (new_http_status_options) {
		char *o = http_status_options;
		http_status_options = new_http_status_options;
		new_http_status_options = NULL;
		if (o)
			hfree(o);
	} else {
		char *o = http_status_options;
		http_status_options = NULL;
		hfree(o);
	}
	
	if (new_fake_version) {
		char *o = fake_version;
		fake_version = new_fake_version;
		new_fake_version = NULL;
		if (o)
			hfree(o);
	} else {
		char *o = fake_version;
		fake_version = NULL;
		hfree(o);
	}
	
	if (new_disallow_srccall_glob) {
		char **o = disallow_srccall_glob;
		disallow_srccall_glob = new_disallow_srccall_glob;
		new_disallow_srccall_glob = NULL;
		if (o)
			free_string_array(o);
	} else if (disallow_srccall_glob) {
		char **o = disallow_srccall_glob;
		disallow_srccall_glob = NULL;
		free_string_array(o);
	}
	
	if (new_disallow_login_glob) {
		char **o = disallow_login_glob;
		disallow_login_glob = new_disallow_login_glob;
		new_disallow_login_glob = NULL;
		if (o)
			free_string_array(o);
	} else if (disallow_login_glob) {
		char **o = disallow_login_glob;
		disallow_login_glob = NULL;
		free_string_array(o);
	}
	
	/* validate uplink config: if there is a single 'multiro' connection
	 * configured, all of the uplinks must be 'multiro'
	 */
	int uplink_config_failed = 0;
	int got_multiro = 0;
	int got_non_multiro = 0;
	struct uplink_config_t *up;
	for (up = new_uplink_config; (up); up = up->next) {
		if (up->client_flags & CLFLAGS_UPLINKMULTI)
			got_multiro = 1;
		else
			got_non_multiro = 1;
		if ((up->client_flags & CLFLAGS_UPLINKMULTI) && !(up->client_flags & CLFLAGS_PORT_RO)) {
			uplink_config_failed = 1;
			hlog(LOG_ERR, "Config: uplink with non-RO MULTI uplink - would cause a loop, not allowed.");
		}
	}
	if ((got_multiro) && (got_non_multiro)) {
		hlog(LOG_ERR, "Config: Configured both multiro and non-multiro uplinks - would cause a loop, not allowed.");
		failed = 1;
		free_uplink_config(&new_uplink_config);
	}
	if (uplink_config_failed)
		free_uplink_config(&new_uplink_config);
	
	if (workers_configured < 1) {
		hlog(LOG_WARNING, "Configured less than 1 worker threads. Using 1.");
	} else if (workers_configured > 32) {
		hlog(LOG_WARNING, "Configured more than 32 worker threads. Using 32.");
		workers_configured = 32;
	}
	
	if (!listen_config_new) {
		hlog(LOG_ERR, "No Listen directives found in configuration.");
		failed = 1;
	} else {
		/* put in the new listening config */
		free_listen_config(&listen_config);
		listen_config = listen_config_new;
		if (listen_config)
			listen_config->prevp = &listen_config;
		listen_config_new = NULL;
	}

	/* put in the new aprsis-uplink  config */
	if (uplink_config_install) {
		// too quick reconfig - uplink thread has not reconfigured yet
		hlog(LOG_WARNING, "New uplink config discarded - too quick reload, Uplink thread has not reconfigured yet");
		free_uplink_config(&new_uplink_config);
	} else {
		uplink_config_install = new_uplink_config;
		new_uplink_config = NULL;
		__sync_synchronize();
		uplink_config_updated = 1;
	}

	/* put in the new aprsis-peerip  config */
	free_peerip_config(&peerip_config);
	peerip_config = new_peerip_config;
	if (peerip_config)
		peerip_config->prevp = &peerip_config;
	new_peerip_config = NULL;
	
	/* put in new http config */
	free_http_config(&http_config);
	http_config = new_http_config;
	if (http_config)
		http_config->prevp = &http_config;
	new_http_config = NULL;
	
	if (failed)
		return -1;
	
	if (!pidfile) {
		s = hmalloc(strlen(log_dir) + 1 + strlen(logname) + 4 + 1);
		sprintf(s, "%s/%s.pid", log_dir, logname);
		
		pidfile = s;
	}
	
	/* log network ID if not default */
	if (q_protocol_id != Q_PROTOCOL_ID_DEFAULT)
		hlog(LOG_INFO, "Q protocol ID: %c - %s", q_protocol_id, (disallow_other_protocol_id) ? "dropping others" : "allowing others");
	
	return 0;
}

/*
 *	Free configuration variables
 */

void free_config(void)
{
	if (log_dir)
		hfree(log_dir);
	log_dir = NULL;
	if (rundir)
		hfree(rundir);
	rundir = NULL;
	if (pidfile)
		hfree(pidfile);
	pidfile = NULL;
	if (cfgfile != def_cfgfile)
		hfree(cfgfile);
	cfgfile = NULL;
	if (logname != def_logname)
		hfree(logname);
	if (webdir != def_webdir)
		hfree(webdir);
	hfree(serverid);
	hfree(passcode);
	hfree(myemail);
	hfree(myadmin);
	serverid = passcode = myemail = myadmin = NULL;
	logname = NULL;
	if (fake_version)
		hfree(fake_version);
	fake_version = NULL;
	free_listen_config(&listen_config);
	free_peerip_config(&peerip_config);
	free_http_config(&http_config);
	if (disallow_srccall_glob) {
		free_string_array(disallow_srccall_glob);
		disallow_srccall_glob = NULL;
	}
	if (disallow_login_glob) {
		free_string_array(disallow_login_glob);
		disallow_login_glob = NULL;
	}
}

