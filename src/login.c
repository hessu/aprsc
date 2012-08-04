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
 *	login.c: works in the context of the worker thread
 */

int login_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len)
{
	int argc;
	char *argv[256];
	int i, rc;
	
	/* make it null-terminated for our string processing */
	char *e = s + len;
	*e = 0;
	hlog(LOG_DEBUG, "%s: login string: '%s' (%d)", c->addr_rem, s, len);
	
	/* parse to arguments */
	if ((argc = parse_args_noshell(argv, s)) == 0 || *argv[0] == '#')
		return 0;
	
	if (argc < 2) {
		hlog(LOG_WARNING, "%s: Invalid login string, too few arguments: '%s'", c->addr_rem, s);
		return 0;
	}
	
	if (strcasecmp(argv[0], "user") != 0) {
		hlog(LOG_WARNING, "%s: Invalid login string, no 'user': '%s'", c->addr_rem, s);
		return 0;
	}
	
	char *username = argv[1];
	if (strlen(username) > 9) /* limit length */
		username[9] = 0;
#ifndef FIXED_IOBUFS
	c->username = hstrdup(username);
#elseu
	strncpy(c->username, username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
#endif
	c->username_len = strlen(c->username);
	
	int given_passcode = -1;
	
	for (i = 2; i < argc; i++) {
		if (strcasecmp(argv[i], "pass") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): No passcode after pass command", c->addr_rem, username);
				break;
			}
			
			given_passcode = atoi(argv[i]);
			if (given_passcode >= 0)
				if (given_passcode == aprs_passcode(c->username))
					c->validated = 1;
		} else if (strcasecmp(argv[i], "vers") == 0) {
			/* Collect application name and version separately.
			 * Some clients only give out application name but
			 * no version. If those same applications do try to
			 * use filter or udp, the filter/udp keyword will end
			 * up as the version number. So good luck with that.
			 */
			 
			if (i+1 >= argc) {
				hlog(LOG_INFO, "%s (%s): No application name after 'vers' in login", c->addr_rem, username);
				break;
			}
			
#ifndef FIXED_IOBUFS
			c->app_version = hstrdup(argv[++i]);
#else
			strncpy(c->app_name,    argv[++i], sizeof(c->app_name));
			c->app_name[sizeof(c->app_name)-1] = 0;
#endif

			if (i+1 >= argc) {
				hlog(LOG_DEBUG, "%s (%s): No application version after 'vers' in login", c->addr_rem, username);
				break;
			}
			
#ifndef FIXED_IOBUFS
			c->app_version = hstrdup(argv[++i]);
#else
			strncpy(c->app_version, argv[++i], sizeof(c->app_version));
			c->app_version[sizeof(c->app_version)-1] = 0;
#endif

		} else if (strcasecmp(argv[i], "udp") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): Missing UDP port number after UDP command", c->addr_rem, username);
				break;
			}
			c->udp_port = atoi(argv[i]);
			if (c->udp_port < 1024 || c->udp_port > 65535) {
				hlog(LOG_WARNING, "%s (%s): UDP port number %s is out of range", c->addr_rem, username, argv[i]);
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
				hlog(LOG_DEBUG, "%s (%s): Requested UDP on client port with no UDP configured", c->addr_rem, username);
				c->udp_port = 0;
				rc = client_printf(self, c, "# No UDP service available on this port\r\n");
				if (rc < -2)
					return rc; // client got destroyed
					
			}

		} else if (strstr(argv[i], "filter")) {
                        /* Follows javaaprssrvr's example - any command having 'filter' in the
                         * end is OK.
                         */
			if (!(c->flags & CLFLAGS_USERFILTEROK)) {
				rc = client_printf(self, c, "# No user-specified filters on this port\r\n");
				if (rc < -2)
                        		return rc; // client got destroyed
				break;
			}
			
			/* copy the null-separated filter arguments back to a space-separated
			 * string, for the status page to show
			 */
			char *fp = c->filter_s;
			char *fe = c->filter_s + FILTER_S_SIZE;
			int f_non_first = 0;
			
			while (++i < argc) {
				int l = strlen(argv[i]);
				if (fp + l + 2 < fe) {
					if (f_non_first) {
						*fp++ = ' ';
					}
					memcpy(fp, argv[i], l);
					fp += l;
					*fp = 0;
					
					f_non_first = 1;	
				}
				
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
	
	/* ok, login succeeded, switch handler */
	c->handler = &incoming_handler; /* handler of all incoming APRS-IS data during a connection */
	if (c->flags & CLFLAGS_UPLINKSIM)
		c->handler = &incoming_uplinksim_handler;
	
	rc = client_printf( self, c, "# logresp %s %s, server %s\r\n",
			    username,
			    (c->validated) ? "verified" : "unverified",
			    serverid );
	if (rc < -2)
		return rc; // The client probably got destroyed!

	c->keepalive = now + keepalive_interval;
	c->state = CSTATE_CONNECTED;
	
	hlog(LOG_DEBUG, "%s: login '%s'%s%s%s%s%s%s%s%s",
	     c->addr_rem, username,
	     (c->validated) ? " pass_ok" : "",
	     (!c->validated && given_passcode >= 0) ? " pass_invalid" : "",
	     (given_passcode < 0) ? " pass_none" : "",
	     (c->udp_port) ? " UDP" : "",
	     (c->app_name) ? " app " : "",
	     (c->app_name) ? c->app_name : "",
	     (c->app_version) ? " ver " : "",
	     (c->app_version) ? c->app_version : ""
	);
	
	/* Add the client to the client list.
	 *
	 * If the client logged in with a valid passcode, check if there are
	 * other validated clients logged in with the same username.
	 * If one is found, it needs to be disconnected.
	 *
	 * The lookup is done while holding the write lock to the clientlist,
	 * instead of a separate lookup call, so that two clients logging in
	 * at exactly the same time won't make it.
	 */
	 
	int old_fd = clientlist_add(c);
	if (c->validated && old_fd != -1) {
		hlog(LOG_DEBUG, "fd %d: Disconnecting duplicate validated client with username '%s'", old_fd, username);
		shutdown(old_fd, SHUT_RDWR);
	}
	
	return 0;
}


