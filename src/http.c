/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 */

/*
 *	http.c: the HTTP server thread, serving status pages and taking position uploads
 */

#include <signal.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <event2/event.h>  
#include <event2/http.h>  
#include <event2/buffer.h>

#if 0
#ifdef HAVE_EVENT2_EVENT_H
#else // LIBEVENT 1.x
#include <event.h>
#include <evhttp.h>
#include <evutil.h>
#endif
#endif

#include "http.h"
#include "config.h"
#include "version.h"
#include "hlog.h"
#include "hmalloc.h"
#include "worker.h"
#include "status.h"
#include "passcode.h"
#include "incoming.h"
#include "counterdata.h"

int http_shutting_down;
int http_reconfiguring;

unsigned long http_requests = 0;

struct http_static_t {
	char	*name;
	char	*filename;
};

struct worker_t *http_worker = NULL;
struct client_t *http_pseudoclient = NULL;

/*
 *	This is a list of files that the http server agrees to serve.
 *	Due to security concerns the list is static.
 *	It's a lot of work to actually implement a full-blown secure web
 *	server, and that's not what we're trying to do here.
 */

static struct http_static_t http_static_files[] = {
	{ "/", "index.html" },
	{ "/favicon.ico", "favicon.ico" },
	{ "/aprsc.css", "aprsc.css" },
	{ "/aprsc.js", "aprsc.js" },
	{ "/aprsc-logo1.png", "aprsc-logo1.png" },
	{ "/aprsc-logo2.png", "aprsc-logo2.png" },
	{ "/excanvas.min.js", "excanvas.min.js" },
	{ "/jquery.flot.min.js", "jquery.flot.min.js" },
	{ NULL, NULL }
};

/*
 *	Content types for the required file extensions
 */

static struct http_static_t http_content_types[] = {
	{ ".html", "text/html; charset=UTF-8" },
	{ ".ico", "image/x-icon" },
	{ ".css", "text/css; charset=UTF-8" },
	{ ".js", "application/x-javascript; charset=UTF-8" },
	{ ".jpg", "image/jpeg" },
	{ ".jpeg", "image/jpeg" },
	{ ".png", "image/png" },
	{ ".gif", "image/gif" },
	{ NULL, NULL }
};

/*
 *	Find a content-type for a file name
 */

char *http_content_type(char *fn)
{
	struct http_static_t *cmdp;
	static char default_ctype[] = "text/html";
	char *s;
	
	s = strrchr(fn, '.');
	if (!s)
		return default_ctype;
		
	for (cmdp = http_content_types; cmdp->name != NULL; cmdp++)
		if (strcasecmp(cmdp->name, s) == 0)
			break;
	
	if (cmdp->name == NULL)
		return default_ctype;
	
	return cmdp->filename;
}

/*
 *	HTTP date formatting
 */

int http_date(char *buf, int len, time_t t)
{
	struct tm tb;
	char *wkday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", NULL };
	char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
			"Sep", "Oct", "Nov", "Dec", NULL };
	
	gmtime_r(&t, &tb);
	
	return snprintf(buf, len, "%s, %02d %s %04d %02d:%02d:%02d GMT",
		wkday[tb.tm_wday], tb.tm_mday, mon[tb.tm_mon], tb.tm_year + 1900,
		tb.tm_hour, tb.tm_min, tb.tm_sec);
}

void http_header_base(struct evkeyvalq *headers, int last_modified)
{
	char dbuf[80];
	
	http_date(dbuf, sizeof(dbuf), tick);
	
	evhttp_add_header(headers, "Server", PROGNAME "/" VERSION);
	evhttp_add_header(headers, "Date", dbuf);
	
	if (last_modified) {
		http_date(dbuf, sizeof(dbuf), last_modified);
		evhttp_add_header(headers, "Last-Modified", dbuf);
	}
}


/*
 *	Parse the login string in a POST
 *	Argh, why are these not in standard POST parameters?
 */

int http_upload_login(char *addr_rem, char *s, char **username)
{
	int argc;
	char *argv[256];
	int i;
	
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
	if (strlen(*username) > 9) /* limit length */
		*username[9] = 0;
	
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
 *	Split a login + packet string. Terminates login string with 0,
 *	returns length of packet.
 */

int loginpost_split(char *post, int len, char **login_string, char **packet)
{
	char *cr, *lf;
	char *pack;
	int packet_len;
	
	// find line feed, terminate string
	lf = memchr(post, '\n', len);
	if (!lf)
		return -1;
	
	*lf = 0;
	
	// find optional carriage return, terminate string
	cr = memchr(post, '\r', lf-post);
	if (cr)
		*cr = 0;
	
	// ok, we have a login string.
	*login_string = post;
	
	// now the first line contains a login string. Go for the packet body, find optional lf:
	pack = lf + 1;
	packet_len = len - (pack - post);
	lf = memchr(pack, '\n', packet_len);
	if (lf) {
		*lf = 0;
		packet_len = lf - pack;
	}
	
	// find optional carriage return, terminate string
	cr = memchr(pack, '\r', packet_len);
	if (cr) {
		*cr = 0;
		packet_len = cr - pack;
	}
	
	*packet = pack;
	
	return packet_len;
}


/*
 *	Accept a POST containing a position
 */

#define MAX_HTTP_POST_DATA 2048

void http_upload_position(struct evhttp_request *r, char *remote_host)
{
	struct evbuffer *bufin, *bufout;
	struct evkeyvalq *req_headers;
	const char *ctype, *clength;
	int clength_i;
	char post[MAX_HTTP_POST_DATA+1];
	ev_ssize_t l;
	char *login_string = NULL;
	char *packet = NULL;
	char validated;
	int e;
	int packet_len;
	
	req_headers = evhttp_request_get_input_headers(r);
	ctype = evhttp_find_header(req_headers, "Content-Type");
	
	if (!ctype || strcasecmp(ctype, "application/octet-stream") != 0) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, wrong or missing content-type");
		return;
	}
	
	clength = evhttp_find_header(req_headers, "Content-Length");
	if (!clength) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, missing content-length");
		return;
	}
	
	clength_i = atoi(clength);
	if (clength_i > MAX_HTTP_POST_DATA) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, too large body");
		return;
	}
	
	/* get the HTTP POST body */
	bufin = evhttp_request_get_input_buffer(r);
	l = evbuffer_copyout(bufin, post, MAX_HTTP_POST_DATA);
	
	/* Just for convenience and safety, null-terminate. Easier to log. */
	post[MAX_HTTP_POST_DATA] = 0;
	if (l <= MAX_HTTP_POST_DATA)
		post[l] = 0;
	
	if (l != clength_i) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Body size does not match content-length");
		return;
	}
	
	hlog(LOG_DEBUG, "got post data: %s", post);
	
	packet_len = loginpost_split(post, l, &login_string, &packet);
	if (packet_len == -1) {
		evhttp_send_error(r, HTTP_BADREQUEST, "No newline (LF) found in data");
		return;
	}
	
	if (!login_string) {
		evhttp_send_error(r, HTTP_BADREQUEST, "No login string in data");
		return;
	}
	
	if (!packet) {
		evhttp_send_error(r, HTTP_BADREQUEST, "No packet data found in data");
		return;
	}
	
	hlog(LOG_DEBUG, "login string: %s", login_string);
	hlog(LOG_DEBUG, "packet: %s", packet);
	
	/* process the login string */
	char *username;
	validated = http_upload_login(remote_host, login_string, &username);
	if (validated < 0) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Invalid login string");
		return;
	}
	
	if (validated != 1) {
		evhttp_send_error(r, 403, "Invalid passcode");
		return;
	}
	
	/* packet size limits */
	if (packet_len < PACKETLEN_MIN) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Packet too short");
		return;
	}
	
	if (packet_len > PACKETLEN_MAX-2) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Packet too long");
		return;
	}
	
	/* fill the user's information in the pseudoclient's structure
	 * for the q construct handler's viewing pleasure
	 */
#ifdef FIXED_IOBUFS
	strncpy(http_pseudoclient->username, username, sizeof(http_pseudoclient->username));
	http_pseudoclient->username[sizeof(http_pseudoclient->username)-1] = 0;
#else
	http_pseudoclient->username = username;
#endif	
	http_pseudoclient->username_len = strlen(http_pseudoclient->username);
	
	/* ok, try to digest the packet */
	e = incoming_parse(http_worker, http_pseudoclient, packet, packet_len);

#ifdef FIXED_IOBUFS
	http_pseudoclient->username[0] = 0;
#else
	http_pseudoclient->username = NULL;
#endif
	
	if (e < 0) {
		hlog(LOG_DEBUG, "http incoming packet parse failure code %d: %s", e, packet);
		evhttp_send_error(r, HTTP_BADREQUEST, "Packet parsing failure");
		return;
	}
	
	/* if the packet parser managed to digest the packet and put it to
	 * the thread-local incoming queue, flush it for dupecheck to
	 * grab
	 */
	if (http_worker->pbuf_incoming_local)
		incoming_flush(http_worker);
	
	bufout = evbuffer_new();
	evbuffer_add(bufout, "ok\n", 3);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, 0);
	evhttp_add_header(headers, "Content-Type", "text/plain; charset=UTF-8");
	
	evhttp_send_reply(r, HTTP_OK, "OK", bufout);
	evbuffer_free(bufout);
}

/*
 *	Generate a status JSON response
 */

void http_status(struct evhttp_request *r)
{
	char *json;
	struct evbuffer *buffer = evbuffer_new();
	
	json = status_json_string(0, 0);
	evbuffer_add(buffer, json, strlen(json));
	free(json);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, tick);
	evhttp_add_header(headers, "Content-Type", "application/json; charset=UTF-8");
	evhttp_add_header(headers, "Cache-Control", "max-age=9");
	
	evhttp_send_reply(r, HTTP_OK, "OK", buffer);
	evbuffer_free(buffer);
}

/*
 *	Return counterdata in JSON
 */

void http_counterdata(struct evhttp_request *r, const char *uri)
{
	char *json;
	const char *query;
	
	query = evhttp_uri_get_query(evhttp_request_get_evhttp_uri(r));
	hlog(LOG_DEBUG, "counterdata query: %s", query);
	
	json = cdata_json_string(query);
	if (!json) {
		evhttp_send_error(r, HTTP_BADREQUEST, "Bad request, no such counter");
		return;
	}
	
	struct evbuffer *buffer = evbuffer_new();
	evbuffer_add(buffer, json, strlen(json));
	free(json);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, tick);
	evhttp_add_header(headers, "Content-Type", "application/json; charset=UTF-8");
	evhttp_add_header(headers, "Cache-Control", "max-age=58");
	
	evhttp_send_reply(r, HTTP_OK, "OK", buffer);
	evbuffer_free(buffer);
}

/*
 *	HTTP static file server
 */

#define HTTP_FNAME_LEN 1024

static void http_route_static(struct evhttp_request *r, const char *uri)
{
	struct http_static_t *cmdp;
	struct stat st;
	char fname[HTTP_FNAME_LEN];
	char last_modified[128];
	char *contenttype;
	int fd;
	int file_size;
	char *buf;
	struct evbuffer *buffer;
	struct evkeyvalq *req_headers;
	const char *ims;
	
	for (cmdp = http_static_files; cmdp->name != NULL; cmdp++)
		if (strcmp(cmdp->name, uri) == 0)
			break;
			
	if (cmdp->name == NULL) {
		hlog(LOG_DEBUG, "HTTP: 404");
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	snprintf(fname, HTTP_FNAME_LEN, "%s/%s", webdir, cmdp->filename);
	
	hlog(LOG_DEBUG, "static file request %s", uri);
	
	if (stat(fname, &st) == -1) {
		hlog(LOG_ERR, "http static file '%s' not found", fname);
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	http_date(last_modified, sizeof(last_modified), st.st_mtime);
	
	contenttype = http_content_type(cmdp->filename);
	//hlog(LOG_DEBUG, "found content-type %s", contenttype);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, st.st_mtime);
	evhttp_add_header(headers, "Content-Type", contenttype);
	
	/* Consider an IMS hit */
	req_headers = evhttp_request_get_input_headers(r);
	ims = evhttp_find_header(req_headers, "If-Modified-Since");
	
	if ((ims) && strcasecmp(ims, last_modified) == 0) {
		hlog(LOG_DEBUG, "http static file '%s' IMS hit", fname);
		evhttp_send_reply(r, HTTP_NOTMODIFIED, "Not modified", NULL);
		return;
	}
	
	file_size = st.st_size;  
	
	fd = open(fname, 0, O_RDONLY);
	if (fd < 0) {
		hlog(LOG_ERR, "http static file '%s' could not be opened for reading: %s", fname, strerror(errno));
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		return;
	}
	
	buf = hmalloc(file_size);
	int n = read(fd, buf, file_size);
	
	if (close(fd) < 0) {
		hlog(LOG_ERR, "http static file '%s' could not be closed after reading: %s", fname, strerror(errno));
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		hfree(buf);
		return;
	}
	
	if (n != file_size) {
		hlog(LOG_ERR, "http static file '%s' could only read %d of %d bytes", fname, n, file_size);
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		hfree(buf);
		return;
	}
	
	buffer = evbuffer_new();
	evbuffer_add(buffer, buf, n);
	hfree(buf);
	
	evhttp_send_reply(r, HTTP_OK, "OK", buffer);
	evbuffer_free(buffer);
}

/*
 *	HTTP request router
 */

void http_router(struct evhttp_request *r, void *which_server)
{
	char *remote_host;
	ev_uint16_t remote_port;
	const char *uri = evhttp_request_get_uri(r);
	struct evhttp_connection *conn = evhttp_request_get_connection(r);
	evhttp_connection_get_peer(conn, &remote_host, &remote_port);
	
	hlog(LOG_DEBUG, "http %s [%s] request %s", (which_server == (void *)1) ? "status" : "upload", remote_host, uri);
	
	http_requests++;
	
	/* status server routing */
	if (which_server == (void *)1) {
		if (strncmp(uri, "/status.json", 12) == 0) {
			http_status(r);
			return;
		}
		
		if (strncmp(uri, "/counterdata?", 13) == 0) {
			http_counterdata(r, uri);
			return;
		}
		
		http_route_static(r, uri);
		return;
	}
	
	/* position upload server routing */
	if (which_server == (void *)2) {
		if (strncmp(uri, "/", 7) == 0) {
			http_upload_position(r, remote_host);
			return;
		}
		
		hlog(LOG_DEBUG, "http request on upload server for '%s': 404 not found", uri);
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	hlog(LOG_ERR, "http request on unknown server for '%s': 404 not found", uri);
	evhttp_send_error(r, HTTP_NOTFOUND, "Server not found");
	
	return;
}

struct event *ev_timer = NULL;
struct evhttp *srvr_status = NULL;
struct evhttp *srvr_upload = NULL;
struct event_base *libbase = NULL;

/*
 *	HTTP timer event, mainly to catch the shutdown signal
 */

void http_timer(evutil_socket_t fd, short events, void *arg)
{
	struct timeval http_timer_tv;
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	//hlog(LOG_DEBUG, "http_timer fired");
	
	if (http_shutting_down) {
		http_timer_tv.tv_usec = 1000;
		event_base_loopexit(libbase, &http_timer_tv);
		return;
	}
	
	event_add(ev_timer, &http_timer_tv);
}

void http_srvr_defaults(struct evhttp *srvr)
{
	// limit what the clients can do a bit
	evhttp_set_allowed_methods(srvr, EVHTTP_REQ_GET);
	evhttp_set_timeout(srvr, 30);
	evhttp_set_max_body_size(srvr, 10*1024);
	evhttp_set_max_headers_size(srvr, 10*1024);
	
	// TODO: How to limit the amount of concurrent HTTP connections?
}

struct client_t *http_pseudoclient_setup(void)
{
	struct client_t *c;
	
	c = http_pseudoclient = client_alloc();
	c->fd    = -1;
	c->portnum = 80;
	c->state = CSTATE_CONNECTED;
	c->flags = CLFLAGS_INPORT|CLFLAGS_CLIENTONLY;
	c->validated = 1; // we will validate on every packet
	//c->portaccount = l->portaccount;
	c->keepalive = tick;
	c->connect_time = tick;
	c->last_read = tick;
	
	return c;
}

/*
 *	HTTP server thread
 */

void http_thread(void *asdf)
{
	sigset_t sigs_to_block;
	struct http_config_t *lc;
	struct timeval http_timer_tv;
	
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	pthreads_profiling_reset("http");
	
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
	
	/* start the http thread, which will start server threads */
	hlog(LOG_INFO, "HTTP thread starting...");
	
	/* we allocate a worker structure to be used within the http thread
	 * for parsing incoming packets and passing them on to the dupecheck
	 * thread.
	 */
	http_worker = worker_alloc();
	http_worker->id = 80;
	
	/* we also need a client structure to be used with incoming
	 * HTTP position uploads
	 */
	http_pseudoclient_setup();
	
	http_reconfiguring = 1;
	while (!http_shutting_down) {
		if (http_reconfiguring) {
			http_reconfiguring = 0;
			
			// shut down existing instance
			if (ev_timer) {
				event_del(ev_timer);
			}
			if (srvr_status) {
				evhttp_free(srvr_status);
				srvr_status = NULL;
			}
			if (srvr_upload) {
				evhttp_free(srvr_upload);
				srvr_upload = NULL;
			}
			if (libbase) {
				event_base_free(libbase);
				libbase = NULL;
			}
			
			// do init
#if 1
			libbase = event_base_new(); // libevent 2.x
#else
                        libbase = event_init(); // libevent 1.x
#endif
			
			// timer for the whole libevent, to catch shutdown signal
			ev_timer = event_new(libbase, -1, EV_TIMEOUT, http_timer, NULL);
			event_add(ev_timer, &http_timer_tv);
			
			for (lc = http_config; (lc); lc = lc->next) {
				hlog(LOG_INFO, "Binding HTTP %s socket %s:%d", lc->upload_port ? "upload" : "status", lc->host, lc->port);
				
				struct evhttp *srvr;
				struct evhttp_bound_socket *handle;
				
				if (lc->upload_port) {
					if (!srvr_upload) {
						srvr_upload = evhttp_new(libbase);
						http_srvr_defaults(srvr_upload);
						evhttp_set_allowed_methods(srvr_upload, EVHTTP_REQ_POST); /* uploads are POSTs, after all */
						evhttp_set_gencb(srvr_upload, http_router, (void *)2);
					}
					srvr = srvr_upload;
				} else {
					if (!srvr_status) {
						srvr_status = evhttp_new(libbase);
						http_srvr_defaults(srvr_status);
						evhttp_set_gencb(srvr_status, http_router, (void *)1);
					}
					srvr = srvr_status;
				}
				
				handle = evhttp_bind_socket_with_handle(srvr, lc->host, lc->port);
				if (!handle) {
					hlog(LOG_ERR, "Failed to bind HTTP socket %s:%d: %s", lc->host, lc->port, strerror(errno));
					// TODO: should exit?
				}
			}
			
			hlog(LOG_INFO, "HTTP thread ready.");
		}
		
		event_base_dispatch(libbase);
	}
	
	/* free up the pseudo-client */
	client_free(http_pseudoclient);
	http_pseudoclient = NULL;
	
	/* free up the pseudo-worker structure */
	/* Well, don't free it here. Dupecheck may SEGV before it shuts down.
	worker_free_buffers(http_worker);
	hfree(http_worker);
	http_worker = NULL;
	*/
	
	hlog(LOG_DEBUG, "HTTP thread shutting down...");
}

