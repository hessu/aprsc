/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
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
	c->username = hstrdup(username);
	c->handler = &incoming_handler; /* handler of all incoming APRS-IS data during a connection */
	
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
			
			c->app_name = hstrdup(argv[++i]);
			c->app_version = hstrdup(argv[++i]);
		} else if (strcasecmp(argv[i], "udp") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): Missing UDP port number after UDP command", c->addr_s, username);
				break;
			}
			c->udp_port = atoi(argv[i]);
			if (c->udp_port < 1 || c->udp_port > 65535 || c->udp_port == 53) {
				hlog(LOG_WARNING, "%s (%s): UDP port number %s is out of range", c->addr_s, username, argv[i]);
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
	
	return 0;
}


