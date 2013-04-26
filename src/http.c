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
#include "login.h"
#include "counterdata.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

/* supported HTTP transfer-encoding methods */
#define HTTP_COMPR_GZIP 1

const char *compr_type_strings[] = {
	"none",
	"gzip",
	"deflate"
};

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
	{ "/motd.html", "motd.html" },
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

static char *http_content_type(const char *fn)
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

static int http_date(char *buf, int len, time_t t)
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

static void http_header_base(struct evkeyvalq *headers, int last_modified)
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
 *	Process an incoming HTTP or UDP packet by parsing it and pushing
 *	it to the dupecheck thread through the pseudoworker
 */

int pseudoclient_push_packet(struct worker_t *worker, struct client_t *pseudoclient, const char *username, char *packet, int packet_len)
{
	int e;
	
	/* fill the user's information in the pseudoclient's structure
	 * for the q construct handler's viewing pleasure
	 */
	strncpy(pseudoclient->username, username, sizeof(pseudoclient->username));
	pseudoclient->username[sizeof(pseudoclient->username)-1] = 0;
	pseudoclient->username_len = strlen(pseudoclient->username);
	
	/* ok, try to digest the packet */
	e = incoming_parse(worker, pseudoclient, packet, packet_len);

	pseudoclient->username[0] = 0;
	pseudoclient->username_len = 0;
	
	if (e < 0)
		return e;
	
	/* if the packet parser managed to digest the packet and put it to
	 * the thread-local incoming queue, flush it for dupecheck to
	 * grab
	 */
	if (worker->pbuf_incoming_local)
		incoming_flush(worker);
	
	return e;
}

/*
 *	Accept a POST containing a position
 */

#define MAX_HTTP_POST_DATA 2048

static void http_upload_position(struct evhttp_request *r, const char *remote_host)
{
	struct evbuffer *bufin, *bufout;
	struct evkeyvalq *req_headers;
	const char *ctype, *clength;
	int clength_i;
	char post[MAX_HTTP_POST_DATA+1];
	ev_ssize_t l;
	char *login_string = NULL;
	char *packet = NULL;
	char *username = NULL;
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
	validated = http_udp_upload_login(remote_host, login_string, &username);
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
	
	e = pseudoclient_push_packet(http_worker, http_pseudoclient, username, packet, packet_len);

	if (e < 0) {
		hlog(LOG_DEBUG, "http incoming packet parse failure code %d: %s", e, packet);
		evhttp_send_error(r, HTTP_BADREQUEST, "Packet parsing failure");
		return;
	}
	
	bufout = evbuffer_new();
	evbuffer_add(bufout, "ok\n", 3);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, 0);
	evhttp_add_header(headers, "Content-Type", "text/plain; charset=UTF-8");
	
	evhttp_send_reply(r, HTTP_OK, "OK", bufout);
	evbuffer_free(bufout);
}

/*
 *	Check if the client will dig a compressed response
 */

#ifdef HAVE_LIBZ
static int http_check_req_compressed(struct evhttp_request *r)
{
	struct evkeyvalq *req_headers;
	const char *accept_enc;
	
	req_headers = evhttp_request_get_input_headers(r);
	accept_enc = evhttp_find_header(req_headers, "Accept-Encoding");
	
	if (!accept_enc)
		return 0;
	
	if (strstr(accept_enc, "gzip") != NULL)
		return HTTP_COMPR_GZIP;
	
	return 0;
}
#endif

/*
 *	gzip compress a buffer
 */

#ifdef HAVE_LIBZ
static int http_compress_gzip(char *in, int ilen, char *out, int ospace)
{
	z_stream ctx;
	
	ctx.zalloc = Z_NULL;
	ctx.zfree = Z_NULL;
	ctx.opaque = Z_NULL;
	
	/* magic 15 bits + 16 enables gzip header generation */
	if (deflateInit2(&ctx, 7, Z_DEFLATED, (15+16), MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
		hlog(LOG_ERR, "http_compress_gzip: deflateInit2 failed");
		return -1;
	}
	
	ctx.next_in = (unsigned char *)in;
	ctx.avail_in = ilen;
	ctx.next_out = (unsigned char *)out;
	ctx.avail_out = ospace;
	
	int ret = deflate(&ctx, Z_FINISH);
	if (ret != Z_STREAM_END && ret != Z_OK) {
		hlog(LOG_ERR, "http_compress_gzip: deflate returned %d instead of Z_STREAM_END", ret);
		(void)deflateEnd(&ctx);
		return -1;
	}
	
	int olen = ospace - ctx.avail_out;
	hlog(LOG_DEBUG, "http_compress_gzip: compressed %d bytes to %d bytes: %.1f %%", ilen, olen, (float)olen / (float)ilen * 100.0);
	
	(void)deflateEnd(&ctx);
	
	return olen;
}
#endif

/*
 *	Transmit an OK HTTP response, given headers and data.
 *	Compress response, if possible.
 */

static void http_send_reply_ok(struct evhttp_request *r, struct evkeyvalq *headers, char *data, int len, int allow_compress)
{
#ifdef HAVE_LIBZ
	char *compr = NULL;
	
	if (len > 100 && allow_compress) {
		/* Consider returning a compressed version */
		int compr_type = http_check_req_compressed(r);
		/*
		if (compr_type)
			hlog(LOG_DEBUG, "http_send_reply_ok, client supports transfer-encoding: %s", compr_type_strings[compr_type]);
		*/
		
		if (compr_type == HTTP_COMPR_GZIP) {
			compr = hmalloc(len);
			int olen = http_compress_gzip(data, len, compr, len);
			/* If compression succeeded, replace buffer with the compressed one and free the
			 * uncompressed one. Add HTTP header to indicate compressed response.
			 */
			if (olen > 0) {
				data = compr;
				len = olen;
				evhttp_add_header(headers, "Content-Encoding", "gzip");
			}
		}
	}
#endif
	
	struct evbuffer *buffer = evbuffer_new();
	evbuffer_add(buffer, data, len);
	
	evhttp_send_reply(r, HTTP_OK, "OK", buffer);
	evbuffer_free(buffer);

#ifdef HAVE_LIBZ
	if (compr)
		hfree(compr);
#endif
}


/*
 *	Generate a status JSON response
 */

static void http_status(struct evhttp_request *r)
{
	char *json;
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, tick);
	evhttp_add_header(headers, "Content-Type", "application/json; charset=UTF-8");
	evhttp_add_header(headers, "Cache-Control", "max-age=9");
	
	json = status_json_string(0, 0);
	http_send_reply_ok(r, headers, json, strlen(json), 1);
	free(json);
}

/*
 *	Return counterdata in JSON
 */

static void http_counterdata(struct evhttp_request *r, const char *uri)
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
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, tick);
	evhttp_add_header(headers, "Content-Type", "application/json; charset=UTF-8");
	evhttp_add_header(headers, "Cache-Control", "max-age=58");
	
	http_send_reply_ok(r, headers, json, strlen(json), 1);
	hfree(json);
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
	
	//hlog(LOG_DEBUG, "static file request %s", uri);
	
	fd = open(fname, 0, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT) {
			/* don't complain about missing motd.html - it's optional. */
			int level = LOG_ERR;
			if (strcmp(cmdp->filename, "motd.html") == 0)
				level = LOG_DEBUG;
			hlog(level, "http static file '%s' not found", fname);
			evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
			return;
		}
		
		hlog(LOG_ERR, "http static file '%s' could not be opened for reading: %s", fname, strerror(errno));
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		return;
	}
	
	if (fstat(fd, &st) == -1) {
		hlog(LOG_ERR, "http static file '%s' could not fstat() after opening: %s", fname, strerror(errno));
		evhttp_send_error(r, HTTP_INTERNAL, "Could not access file");
		if (close(fd) < 0)
			hlog(LOG_ERR, "http static file '%s' could not be closed after failed stat: %s", fname, strerror(errno));
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
		if (close(fd) < 0)
			hlog(LOG_ERR, "http static file '%s' could not be closed after failed stat: %s", fname, strerror(errno));
		return;
	}
	
	file_size = st.st_size;
	
	/* yes, we are not going to serve large files. */
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
	
	int allow_compress;
	if (strncmp(contenttype, "image/", 6) == 0)
		allow_compress = 0;
	else
		allow_compress = 1;
	
	http_send_reply_ok(r, headers, buf, n, allow_compress);
	hfree(buf);
}

/*
 *	HTTP request router
 */

static void http_router(struct evhttp_request *r, void *which_server)
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

static void http_timer(evutil_socket_t fd, short events, void *arg)
{
	struct timeval http_timer_tv;
	http_timer_tv.tv_sec = 0;
	http_timer_tv.tv_usec = 200000;
	
	//hlog(LOG_DEBUG, "http_timer fired");
	
	if (http_shutting_down || http_reconfiguring) {
		http_timer_tv.tv_usec = 1000;
		event_base_loopexit(libbase, &http_timer_tv);
		return;
	}
	
	event_add(ev_timer, &http_timer_tv);
}

static void http_srvr_defaults(struct evhttp *srvr)
{
	// limit what the clients can do a bit
	evhttp_set_allowed_methods(srvr, EVHTTP_REQ_GET);
	evhttp_set_timeout(srvr, 30);
	evhttp_set_max_body_size(srvr, 10*1024);
	evhttp_set_max_headers_size(srvr, 10*1024);
	
	// TODO: How to limit the amount of concurrent HTTP connections?
}

static void http_server_free(void)
{
	if (ev_timer) {
		event_del(ev_timer);
		hfree(ev_timer);
		ev_timer = NULL;
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
	http_pseudoclient = pseudoclient_setup(80);
	
	http_reconfiguring = 1;
	while (!http_shutting_down) {
		if (http_reconfiguring) {
			http_reconfiguring = 0;
			
			// shut down existing instance
			http_server_free();
			
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
	
	hlog(LOG_DEBUG, "HTTP thread shutting down...");
	
	http_server_free();
	
	/* free up the pseudo-client */
	client_free(http_pseudoclient);
	http_pseudoclient = NULL;
	
	/* free up the pseudo-worker structure */
	worker_free_buffers(http_worker);
	hfree(http_worker);
	http_worker = NULL;
}

