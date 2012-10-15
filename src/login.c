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
#include "parse_qc.h"

/* a static list of usernames which are not allowed to log in */
static const char *disallow_login_usernames[] = {
	"pass", /* a sign of "user  pass -1" login with no configured username */
	NULL
};

/*
 *	Parse the login string in a HTTP POST or UDP submit packet
 *	Argh, why are these not in standard POST parameters in HTTP?
 */

int http_udp_upload_login(const char *addr_rem, char *s, char **username)
{
	int argc;
	char *argv[256];
	int i;
	int username_len;
	
	/* parse to arguments */
	if ((argc = parse_args_noshell(argv, s)) == 0)
		return -1;
	
	if (argc < 2) {
		hlog(LOG_WARNING, "%s: HTTP POST: Invalid login string, too few arguments: '%s'", addr_rem, s);
		return -1;
	}
	
	if (strcasecmp(argv[0], "user") != 0) {
		hlog(LOG_WARNING, "%s: HTTP POST: Invalid login string, no 'user': '%s'", addr_rem, s);
		return -1;
	}
	
	*username = argv[1];
	username_len = strlen(*username);
	/* limit username length */
	if (username_len > CALLSIGNLEN_MAX) {
		hlog(LOG_WARNING, "%s: HTTP POST: Invalid login string, too long 'user' username: '%s'", addr_rem, *username);
		return -1;
	}
	
	/* check the username against a static list of disallowed usernames */
	for (i = 0; (disallow_login_usernames[i]); i++) {
		if (strcasecmp(*username, disallow_login_usernames[i]) == 0) {
			hlog(LOG_WARNING, "%s: HTTP POST: Login by user '%s' not allowed", addr_rem, *username);
			return -1;
		}
	}
	
	/* make sure the callsign is OK on the APRS-IS */
	if (check_invalid_q_callsign(*username, username_len)) {
		hlog(LOG_WARNING, "%s: HTTP POST: Invalid login string, invalid 'user': '%s'", addr_rem, *username);
		return -1;
	}
	
	int given_passcode = -1;
	int validated = 0;
	
	for (i = 2; i < argc; i++) {
		if (strcasecmp(argv[i], "pass") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): HTTP POST: No passcode after pass command", addr_rem, username);
				break;
			}
			
			given_passcode = atoi(argv[i]);
			if (given_passcode >= 0)
				if (given_passcode == aprs_passcode(*username))
					validated = 1;
		} else if (strcasecmp(argv[i], "vers") == 0) {
			if (i+2 >= argc) {
				hlog(LOG_DEBUG, "%s (%s): HTTP POST: No application name and version after vers command", addr_rem, username);
				break;
			}
			
			// skip app name and version
			i += 2;
		}
	}
	
	return validated;
}


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
		rc = client_printf(self, c, "# Invalid login string, too few arguments\r\n");
		goto failed_login;
	}
	
	if (strcasecmp(argv[0], "user") != 0) {
		if (strcasecmp(argv[0], "GET") == 0)
			c->failed_cmds = 10; /* bail out right away for a HTTP client */
		
		c->failed_cmds++;
		hlog(LOG_WARNING, "%s: Invalid login string, no 'user': '%s'", c->addr_rem, s);
		rc = client_printf(self, c, "# Invalid login command\r\n");
		goto failed_login;
	}
	
	char *username = argv[1];
	
	/* limit username length */
	if (strlen(username) > CALLSIGNLEN_MAX) {
		hlog(LOG_WARNING, "%s: Invalid login string, too long 'user' username: '%s'", c->addr_rem, c->username);
		username[CALLSIGNLEN_MAX] = 0;
		rc = client_printf(self, c, "# Invalid username format\r\n");
		goto failed_login;
	}
	
#ifndef FIXED_IOBUFS
	c->username = hstrdup(username);
#else
	strncpy(c->username, username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
#endif
	c->username_len = strlen(c->username);
	
	/* check the username against a static list of disallowed usernames */
	for (i = 0; (disallow_login_usernames[i]); i++) {
		if (strcasecmp(c->username, disallow_login_usernames[i]) == 0) {
			hlog(LOG_WARNING, "%s: Login by user '%s' not allowed", c->addr_rem, c->username);
			rc = client_printf(self, c, "# Login by user not allowed\r\n");
			goto failed_login;
		}
	}
	
	/* make sure the callsign is OK on the APRS-IS */
	if (check_invalid_q_callsign(c->username, c->username_len)) {
		hlog(LOG_WARNING, "%s: Invalid login string, invalid 'user': '%s'", c->addr_rem, c->username);
		rc = client_printf(self, c, "# Invalid username format\r\n");
		goto failed_login;
	}
	
	int given_passcode = -1;
	
	for (i = 2; i < argc; i++) {
		if (strcasecmp(argv[i], "pass") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s/%s: No passcode after pass command", c->addr_rem, username);
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
				hlog(LOG_INFO, "%s/%s: No application name after 'vers' in login", c->addr_rem, username);
				break;
			}
			
#ifndef FIXED_IOBUFS
			c->app_version = hstrdup(argv[++i]);
#else
			strncpy(c->app_name,    argv[++i], sizeof(c->app_name));
			c->app_name[sizeof(c->app_name)-1] = 0;
#endif
			sanitize_ascii_string(c->app_name);

			if (i+1 >= argc) {
				hlog(LOG_DEBUG, "%s/%s: No application version after 'vers' in login", c->addr_rem, username);
				break;
			}
			
#ifndef FIXED_IOBUFS
			c->app_version = hstrdup(argv[++i]);
#else
			strncpy(c->app_version, argv[++i], sizeof(c->app_version));
			c->app_version[sizeof(c->app_version)-1] = 0;
#endif
			sanitize_ascii_string(c->app_version);

		} else if (strcasecmp(argv[i], "udp") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s/%s: Missing UDP port number after UDP command", c->addr_rem, username);
				break;
			}
			c->udp_port = atoi(argv[i]);
			if (c->udp_port < 1024 || c->udp_port > 65535) {
				hlog(LOG_WARNING, "%s/%s: UDP port number %s is out of range", c->addr_rem, username, argv[i]);
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
				
				inbound_connects_account(3, c->udpclient->portaccount); /* "3" = udp, not listening..  */
			} else {
				/* Sorry, no UDP service for this port.. */
				hlog(LOG_DEBUG, "%s/%s: Requested UDP on client port with no UDP configured", c->addr_rem, username);
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
	
	rc = client_printf( self, c, "# logresp %s %s, server %s\r\n",
			    username,
			    (c->validated) ? "verified" : "unverified",
			    serverid );
	if (rc < -2)
		return rc; // The client probably got destroyed!

	c->keepalive = now + keepalive_interval/2 + random() % keepalive_interval;
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
		hlog(LOG_INFO, "fd %d: Disconnecting duplicate validated client with username '%s'", old_fd, username);
		/* The other client may be on another thread, so cannot client_close() it.
		 * There is a small potential race here, if the old client disconnected and
		 * the fd was recycled for another client right after the clientlist check.
		 */
		shutdown(old_fd, SHUT_RDWR);
	}
	
	return 0;

failed_login:
	
	/* if we already lost the client, just return */
	if (rc < -2)
		return rc;
	
	c->failed_cmds++;
	if (c->failed_cmds >= 3) {
		client_close(self, c, CLIERR_LOGIN_RETRIES);
		return -3;
	}
	
	return rc;
}


