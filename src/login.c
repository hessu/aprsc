/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "login.h"
#include "hmalloc.h"
#include "hlog.h"
#include "passcode.h"
#include "incoming.h"
#include "config.h"
#include "filter.h"
#include "clientlist.h"

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
 *	login.c: works in the context of the worker thread
 */

int login_handler(struct worker_t *self, struct client_t *c, char *s, int len)
{
	int argc;
	char *argv[256];
	int i, rc;
	
	/* make it null-terminated for our string processing */
	char *e = s + len;
	*e = 0;
	hlog(LOG_DEBUG, "%s: login string: '%s' (%d)", c->addr_s, s, len);
	
	/* parse to arguments */
	if ((argc = parse_args_noshell(argv, s)) == 0 || *argv[0] == '#')
		return 0;
	
	if (argc < 2) {
		hlog(LOG_WARNING, "%s: Invalid login string, too few arguments: '%s'", c->addr_s, s);
		return 0;
	}
	
	if (strcasecmp(argv[0], "user") != 0) {
		hlog(LOG_WARNING, "%s: Invalid login string, no 'user': '%s'", c->addr_s, s);
		return 0;
	}
	
	char *username = argv[1];
	if (strlen(username) > 9) /* limit length */
		username[9] = 0;
	c->state = CSTATE_CONNECTED;
#ifndef FIXED_IOBUFS
	c->username = hstrdup(username);
#else
	strncpy(c->username, username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
#endif
	c->handler = &incoming_handler; /* handler of all incoming APRS-IS data during a connection */
	if (c->flags & CLFLAGS_UPLINKSIM)
		c->handler = &incoming_uplinksim_handler;
	
	int given_passcode = -1;
	
	for (i = 2; i < argc; i++) {
		if (strcasecmp(argv[i], "pass") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): No passcode after pass command", c->addr_s, username);
				break;
			}
			
			given_passcode = atoi(argv[i]);
			if (given_passcode >= 0)
				if (given_passcode == aprs_passcode(c->username))
					c->validated = 1;
		} else if (strcasecmp(argv[i], "vers") == 0) {
			if (i+2 >= argc) {
				hlog(LOG_WARNING, "%s (%s): No application name and version after vers command", c->addr_s, username);
				break;
			}
			
#ifndef FIXED_IOBUFS
			c->app_name = hstrdup(argv[++i]);
			c->app_version = hstrdup(argv[++i]);
#else
			strncpy(c->app_name,    argv[++i], sizeof(c->app_name));
			c->app_name[sizeof(c->app_name)-1] = 0;
			strncpy(c->app_version, argv[++i], sizeof(c->app_version));
			c->app_version[sizeof(c->app_version)-1] = 0;
#endif
		} else if (strcasecmp(argv[i], "udp") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): Missing UDP port number after UDP command", c->addr_s, username);
				break;
			}
			c->udp_port = atoi(argv[i]);
			if (c->udp_port < 1024 || c->udp_port > 65535) {
				hlog(LOG_WARNING, "%s (%s): UDP port number %s is out of range", c->addr_s, username, argv[i]);
				c->udp_port = 0;
			}

			if (c->udpclient) {
				c->udpaddr = c->addr;
				if (c->udpaddr.sa.sa_family == AF_INET) {
					c->udpaddr.si.sin_port = htons(c->udp_port);
					c->udpaddrlen = sizeof(c->udpaddr.si);
				} else {
					c->udpaddr.si6.sin6_port = htons(c->udp_port);
					c->udpaddrlen = sizeof(c->udpaddr.si6);
				}
			} else {
				/* Sorry, no UDP service for this port.. */
				c->udp_port = 0;
			}

		} else if (strcasecmp(argv[i], "filter") == 0) {
			if (!(c->flags & CLFLAGS_USERFILTEROK)) {
				return client_printf(self, c, "# No user-specified filters on this port\r\n");
			}
			while (++i < argc) {
				/* parse filters in argv[i] */
				rc = filter_parse(c, argv[i], 1);
				if (rc) {
					rc = client_printf( self, c, "# Parse errors on filter spec: '%s'\r\n", argv[i]);
					if (rc < -2)
						return rc; // The client probably got destroyed!
				}
			}
		}
	}
	
	rc = client_printf( self, c, "# logresp %s %s, server %s\r\n",
			    username,
			    (c->validated) ? "verified" : "unverified",
			    mycall );
	if (rc < -2) {
		return i; // The client probably got destroyed!
	}

	c->keepalive = now + keepalive_interval;

	
	hlog(LOG_DEBUG, "%s: login '%s'%s%s%s%s%s%s%s%s",
	     c->addr_s, username,
	     (c->validated) ? " pass_ok" : "",
	     (!c->validated && given_passcode >= 0) ? " pass_invalid" : "",
	     (given_passcode < 0) ? " pass_none" : "",
	     (c->udp_port) ? " UDP" : "",
	     (c->app_name) ? " app " : "",
	     (c->app_name) ? c->app_name : "",
	     (c->app_version) ? " ver " : "",
	     (c->app_version) ? c->app_version : ""
	);
	
	/* add the client to the client list */
	clientlist_add(c);
	
	return 0;
}


