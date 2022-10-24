/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#define _GNU_SOURCE

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
#include "tls.h"

/* a static list of usernames which are not allowed to log in */
static const char *disallow_login_usernames[] = {
	"pass", /* a sign of "user  pass -1" login with no configured username */
	NULL
};

/* a static list of unmaintained applications which receive some
 * special "treatment"
 */
static const char *quirks_mode_blacklist[] = {
	"HR-IXPWIND", /* Haute Networks HauteWIND: transmits LF NUL for line termination */
	"HR-IXP-WIND", /* Variation of Haute Networks HauteWIND */
	"IXP-WIND", /* Variation of Haute Networks HauteWIND */
	"Oww/", /* One-Wire Weather CWOP client sends a NUL byte in beginning of packet (in end of previous line) */
	NULL
};

/*
 *	Parse the login string in a HTTP POST or UDP submit packet
 *	Argh, why are these not in standard POST parameters in HTTP?
 *
 *	TODO: Used for UDP too, so should not say HTTP in log errors...
 */

int http_udp_upload_login(const char *addr_rem, char *s, char **username, const char *log_source)
{
	int argc;
	char *argv[256];
	int i;
	int username_len;
	
	/* parse to arguments */
	if ((argc = parse_args_noshell(argv, s)) == 0)
		return -1;
	
	if (argc < 2) {
		hlog(LOG_WARNING, "%s: %s: Invalid login string, too few arguments: '%s'", addr_rem, log_source, s);
		return -1;
	}
	
	if (strcasecmp(argv[0], "user") != 0) {
		hlog(LOG_WARNING, "%s: %s: Invalid login string, no 'user': '%s'", addr_rem, log_source, s);
		return -1;
	}
	
	*username = argv[1];
	username_len = strlen(*username);
	/* limit username length */
	if (username_len > CALLSIGNLEN_MAX) {
		hlog(LOG_WARNING, "%s: %s: Invalid login string, too long 'user' username: '%s'", addr_rem, log_source, *username);
		return -1;
	}
	
	/* check the username against a static list of disallowed usernames */
	for (i = 0; (disallow_login_usernames[i]); i++) {
		if (strcasecmp(*username, disallow_login_usernames[i]) == 0) {
			hlog(LOG_WARNING, "%s: %s: Login by user '%s' not allowed", addr_rem, log_source, *username);
			return -1;
		}
	}
	
	/* check the username against a dynamic list of disallowed usernames */
	if (disallow_login_glob && check_call_glob_match(disallow_login_glob, *username, username_len)) {
		hlog(LOG_WARNING, "%s: %s: Login by user '%s' not allowed due to config", addr_rem, log_source, *username);
		return -1;
	}
	
	/* make sure the callsign is OK on the APRS-IS */
	if (check_invalid_q_callsign(*username, username_len)) {
		hlog(LOG_WARNING, "%s: %s: Invalid login string, invalid 'user': '%s'", addr_rem, log_source, *username);
		return -1;
	}
	
	int given_passcode = -1;
	int validated = 0;
	
	for (i = 2; i < argc; i++) {
		if (strcasecmp(argv[i], "pass") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s (%s): %s: No passcode after pass command", addr_rem, log_source, username);
				break;
			}
			
			given_passcode = atoi(argv[i]);
			if (given_passcode >= 0)
				if (given_passcode == aprs_passcode(*username))
					validated = 1;
		} else if (strcasecmp(argv[i], "vers") == 0) {
			if (i+2 >= argc) {
				hlog(LOG_DEBUG, "%s (%s): %s: No application name and version after vers command", addr_rem, username, log_source);
				break;
			}
			
			// skip app name and version
			i += 2;
		}
	}
	
	return validated;
}

/*
 *	Check if string haystack starts with needle, return 1 if true
 */

static int prefixmatch(const char *haystack, const char *needle)
{
	do {
		if (*needle == 0)
			return 1; /* we're at the end of the needle, and no mismatches found */
		
		if (*haystack == 0)
			return 0; /* haystack is shorter than needle, cannot match */
		
		if (*haystack != *needle)
			return 0; /* mismatch found... */
		
		/* advance pointers */
		haystack++;
		needle++;
	} while (1);
}

/*
 *	Set and sanitize application name and version strings
 */

void login_set_app_name(struct client_t *c, const char *app_name, const char *app_ver)
{
	int i;
	
	strncpy(c->app_name, app_name, sizeof(c->app_name));
	c->app_name[sizeof(c->app_name)-1] = 0;
	sanitize_ascii_string(c->app_name);
			
	strncpy(c->app_version, app_ver, sizeof(c->app_version));
	c->app_version[sizeof(c->app_version)-1] = 0;
	sanitize_ascii_string(c->app_version);
	
	/* check the application name against a static list of broken apps */
	if (quirks_mode) {
        	c->quirks_mode = 1;
		return;
	}
	
	c->quirks_mode = 0;
	for (i = 0; (quirks_mode_blacklist[i]); i++) {
		if (prefixmatch(c->app_name, quirks_mode_blacklist[i])) {
			hlog(LOG_DEBUG, "%s/%s: Enabling quirks mode for application %s %s",
				c->addr_rem, c->username, c->app_name, c->app_version);
			c->quirks_mode = 1;
			break;
		}
	}
	
}

int login_setup_udp_feed(struct client_t *c, int port)
{
	if (!c->udpclient)
		return -1;
	
	c->udp_port = port;
	c->udpaddr = c->addr;
	if (c->udpaddr.sa.sa_family == AF_INET) {
		c->udpaddr.si.sin_port = htons(c->udp_port);
		c->udpaddrlen = sizeof(c->udpaddr.si);
	} else {
		c->udpaddr.si6.sin6_port = htons(c->udp_port);
		c->udpaddrlen = sizeof(c->udpaddr.si6);
	}
	
	inbound_connects_account(3, c->udpclient->portaccount); /* "3" = udp, not listening..  */
	
	return 0;
}

#ifdef USE_SSL
static int login_client_validate_cert(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_DEBUG, "%s/%s: login: doing TLS client cert validation", c->addr_rem, c->username);
	int ssl_res = ssl_validate_peer_cert_phase1(c);
	if (ssl_res == 0)
		ssl_res = ssl_validate_peer_cert_phase2(c);
	
	if (ssl_res == 0) {
		c->validated = VALIDATED_STRONG;
		return 1;
	}
	
	hlog(LOG_WARNING, "%s/%s: TLS client cert validation failed: %s", c->addr_rem, c->username, ssl_strerror(ssl_res));
	int rc;
	if (ssl_res == SSL_VALIDATE_CLIENT_CERT_UNVERIFIED)
		rc = client_printf(self, c, "# Client certificate not accepted: %s\r\n", X509_verify_cert_error_string(c->ssl_con->ssl_err_code));
	else
		rc = client_printf(self, c, "# Client certificate authentication failed: %s\r\n", ssl_strerror(ssl_res));
		
	c->failed_cmds = 10; /* bail out right away for a HTTP client */
	
	if (rc < 0)
		return rc;
	
	return 0;
}
#endif

/*
 *	login.c: works in the context of the worker thread
 */

int login_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len)
{
	int argc;
	char *argv[256];
	int i;
	int rc = 0;
	
	/* make it null-terminated for our string processing */
	/* TODO: do not modify incoming stream - make s a const char! */
	char *e = s + len;
	*e = 0;
	hlog_packet(LOG_DEBUG, s, len, "%s: login string: ", c->addr_rem);
	
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
		hlog(LOG_WARNING, "%s: Invalid login string, too long 'user' username: '%s'", c->addr_rem, username);
		username[CALLSIGNLEN_MAX] = 0;
		rc = client_printf(self, c, "# Invalid username format\r\n");
		goto failed_login;
	}
	
	/* ok, it's somewhat valid, write it down */
	strncpy(c->username, username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	c->username_len = strlen(c->username);
	
	/* check the username against a static list of disallowed usernames */
	for (i = 0; (disallow_login_usernames[i]); i++) {
		if (strcasecmp(c->username, disallow_login_usernames[i]) == 0) {
			hlog(LOG_WARNING, "%s: Login by user '%s' not allowed", c->addr_rem, c->username);
			rc = client_printf(self, c, "# Login by user not allowed\r\n");
			goto failed_login;
		}
	}
	
	/* check the username against a dynamic list of disallowed usernames */
	if (disallow_login_glob && check_call_glob_match(disallow_login_glob, c->username, c->username_len)) {
		hlog(LOG_WARNING, "%s: Login by user '%s' not allowed due to config", c->addr_rem, c->username);
		rc = client_printf(self, c, "# Login by user not allowed\r\n");
		goto failed_login;
	}
	
	/* make sure the callsign is OK on the APRS-IS */
	if (check_invalid_q_callsign(c->username, c->username_len)) {
		hlog(LOG_WARNING, "%s: Invalid login string, invalid 'user': '%s'", c->addr_rem, c->username);
		rc = client_printf(self, c, "# Invalid username format, not allowed\r\n");
		goto failed_login;
	}
	
	/* make sure the client's callsign is not my Server ID */
	if (strcasecmp(c->username, serverid) == 0) {
		hlog(LOG_WARNING, "%s: Invalid login string, username equals our serverid: '%s'", c->addr_rem, c->username);
		rc = client_printf(self, c, "# Login by user not allowed (our serverid)\r\n");
		goto failed_login;
	}
	
	/* if SSL client cert verification is enabled, check it */
	int ssl_validated = 0;
#ifdef USE_SSL
	if (c->ssl_con && c->ssl_con->validate) {
		ssl_validated = login_client_validate_cert(self, c);
		if (ssl_validated == -1) {
			rc = ssl_validated;
			goto failed_login;
		}
		if (ssl_validated != 1)
			goto failed_login;
	}
#endif
	
	char *pass = NULL;
	
	for (i = 2; i < argc; i++) {
		if (strcasecmp(argv[i], "pass") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s/%s: No passcode after pass command", c->addr_rem, username);
				break;
			}
			
			pass = argv[i];
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
			
			login_set_app_name(c, argv[i+1], (i+2 < argc) ? argv[i+2] : "");
			i += 2;

		} else if (strcasecmp(argv[i], "udp") == 0) {
			if (++i >= argc) {
				hlog(LOG_WARNING, "%s/%s: Missing UDP port number after UDP command", c->addr_rem, username);
				break;
			}
			
			int udp_port = atoi(argv[i]);
			if (udp_port < 1024 || udp_port > 65535) {
				hlog(LOG_WARNING, "%s/%s: UDP port number %s is out of range", c->addr_rem, username, argv[i]);
				break;
			}

			if (login_setup_udp_feed(c, udp_port) != 0) {
				/* Sorry, no UDP service for this port.. */
				hlog(LOG_DEBUG, "%s/%s: Requested UDP on client port with no UDP configured", c->addr_rem, username);
				rc = client_printf(self, c, "# No UDP service available on this port\r\n");
				if (rc < -2)
					return rc; // client got destroyed
					
			}

		} else if (strcasestr(argv[i], "filter")) {
                        /* Follows javaaprssrvr's example - any command having 'filter' in the
                         * end is OK. Case insensitive.
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
	
	/* passcode auth? */
	int given_passcode = -1;
	if (!ssl_validated && (pass)) {
		given_passcode = atoi(pass);
		if (given_passcode >= 0)
			if (given_passcode == aprs_passcode(c->username))
				c->validated = VALIDATED_WEAK;
	}
	
	/* clean up the filter string so that it doesn't contain invalid
	 * UTF-8 or other binary stuff. */
	sanitize_ascii_string(c->filter_s);
	
	/* ok, login succeeded, switch handler */
	c->handler_line_in = &incoming_handler; /* handler of all incoming APRS-IS data during a connection */
	
	rc = client_printf( self, c, "# logresp %s %s, server %s\r\n",
			    username,
			    (c->validated) ? "verified" : "unverified",
			    serverid );
	if (rc < -2)
		return rc; // The client probably got destroyed!
	
	hlog(LOG_DEBUG, "%s: login '%s'%s%s%s%s%s%s%s%s",
	     c->addr_rem, username,
	     (c->validated) ? " pass_ok" : "",
	     (!c->validated && given_passcode >= 0) ? " pass_invalid" : "",
	     (given_passcode < 0) ? " pass_none" : "",
	     (c->udp_port) ? " UDP" : "",
	     (*c->app_name) ? " app " : "",
	     (*c->app_name) ? c->app_name : "",
	     (*c->app_version) ? " ver " : "",
	     (*c->app_version) ? c->app_version : ""
	);
	
	/* mark as connected and classify */
	worker_mark_client_connected(self, c);
	
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
		/* TODO: If old connection is SSL validated, and this one is not, do not disconnect it. */
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


