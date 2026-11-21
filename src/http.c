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
#include <glob.h>

#include <event2/event.h>  
#include <event2/http.h>  
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/keyvalq_struct.h>

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
int http_config_applied = -1; /* the config_generation applied by the http thread, -1 before the first */

unsigned long http_requests = 0;
unsigned long http_connections = 0;		/* connections accepted */
unsigned long http_connections_rejected = 0;	/* ... and closed right away for being over the limit */
int http_clients = 0;				/* connections open now */
int http_clients_max = 0;			/* the limit in effect */

/*
 *	Per-connection context. libevent 2.1 has no hook on the accept of an
 *	HTTP server connection which would give the connection itself: the
 *	bufferevent callback runs before the connection is created, and the
 *	first request callback only when a request has been read, which an
 *	idle connection never sends. So the connection is picked up on the
 *	next round of the event loop, from the callback argument the server
 *	has set on the bufferevent by then, which is the connection the
 *	bufferevent belongs to. A close callback on the connection then
 *	counts it out.
 */
struct http_conn_t {
	struct bufferevent *bev;
	struct event *attach_ev;	/* the timer which picks up the connection */
	struct http_conn_t *next;	/* on the list of connections waiting for it */
	struct http_conn_t **prevp;
};

static struct http_conn_t *http_conn_attaching = NULL;

/* the listeners of the HTTP servers, for pausing them all when accept()
 * runs out of file descriptors */
struct http_listener_t {
	struct evconnlistener *listener;
	struct http_listener_t *next;
};

static struct http_listener_t *http_listeners = NULL;
static struct event *ev_listener_resume = NULL;

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
	{ "/aprsc-graph.js", "aprsc-graph.js" },
	/* allow old index.html versions to load the new logo */
	{ "/aprsc-logo4.png", "aprsc-logo4.png" },
	{ "/aprsc-logo4@2x.png", "aprsc-logo4@2x.png" },
	{ "/aprsc-joulukissa.jpg", "aprsc-joulukissa.jpg" },
	{ "/excanvas.min.js", "excanvas.min.js" },
	{ "/jquery.flot.min.js", "jquery.flot.min.js" },
	{ "/jquery.flot.time.min.js", "jquery.flot.time.min.js" },
	{ "/jquery.flot.selection.min.js", "jquery.flot.selection.min.js" },
	{ "/jquery.flot.resize.min.js", "jquery.flot.resize.min.js" },
	{ "/motd.html", "motd.html" },
	{ "/jquery.min.js", "jquery.min.js" },
	{ "/angular.min.js", "angular.min.js" },
	{ "/angular-translate.min.js", "angular-translate.min.js" },
	{ "/angular-translate-loader-url.min.js", "angular-translate-loader-url.min.js" },
	{ "/ngDialog.min.js", "ngDialog.min.js" },
	{ "/ngDialog.min.css", "ngDialog.min.css" },
	{ "/ngDialog-theme-plain.min.css", "ngDialog-theme-plain.min.css" },
	{ "/bootstrap.min.css", "bootstrap.min.css" },
	{ "/fonts/glyphicons-halflings-regular.eot", "glyphicons-halflings-regular.eot" },
	{ "/fonts/glyphicons-halflings-regular.ttf", "glyphicons-halflings-regular.ttf" },
	{ "/fonts/glyphicons-halflings-regular.woff", "glyphicons-halflings-regular.woff" },
	{ "/fonts/glyphicons-halflings-regular.woff2", "glyphicons-halflings-regular.woff2" },
	{ NULL, NULL }
};

int http_language_count = 0;
struct http_static_t **http_language_files = NULL;

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
	{ ".json", "application/json" },
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
	const char *s;
	
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
	
	evhttp_add_header(headers, "Server", verstr_http);
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
	e = incoming_parse(worker, pseudoclient, packet, packet_len, NULL);

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
	int validated;
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
	validated = http_udp_upload_login(remote_host, login_string, &username, "HTTP POST");
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

	memset(&ctx, 0, sizeof(ctx));
	
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
	if (ret != Z_STREAM_END) {
		hlog(LOG_ERR, "http_compress_gzip: deflate returned %d instead of Z_STREAM_END", ret);
		(void)deflateEnd(&ctx);
		return -1;
	}
	
	int olen = ospace - ctx.avail_out;
	//hlog(LOG_DEBUG, "http_compress_gzip: compressed %d bytes to %d bytes: %.1f %%", ilen, olen, (float)olen / (float)ilen * 100.0);
	
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
	
	/* Gzipping files below 150 bytes can actually make them larger. */
	if (len > 150 && allow_compress) {
		/* Consider returning a compressed version */
		int compr_type = http_check_req_compressed(r);
		/*
		if (compr_type)
			hlog(LOG_DEBUG, "http_send_reply_ok, client supports transfer-encoding: %s", compr_type_strings[compr_type]);
		*/
		
		if (compr_type == HTTP_COMPR_GZIP) {
			/* for small files it's possible that the output is actually
			 * larger than the input
			 */
			int oblen = len + 60;
			compr = hmalloc(oblen);
			int olen = http_compress_gzip(data, len, compr, oblen);
			/* If compression succeeded, replace buffer with the compressed one and free the
			 * uncompressed one. Add HTTP header to indicate compressed response.
			 * If the file got larger, send uncompressed.
			 */
			if (olen > 0 && olen < len) {
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
	hlog(LOG_DEBUG, "http counterdata query: %s", query);
	
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

static void http_static_file(struct evhttp_request *r, const char *fname)
{
	struct stat st;
	int fd;
	int file_size;
	struct evkeyvalq *req_headers;
	const char *ims;
	char last_modified[128];
	char *contenttype;
	char *buf;
	
	fd = open(fname, 0, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT) {
			/* don't complain about missing motd.html - it's optional. */
			int level = LOG_ERR;
			if (strcmp(fname, "motd.html") == 0)
				level = LOG_DEBUG;
			hlog(level, "http static file '%s' not found: 404", fname);
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
	
	contenttype = http_content_type(fname);
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


#define HTTP_FNAME_LEN 1024

static void http_route_static(struct evhttp_request *r, const char *uri)
{
	struct http_static_t *cmdp;
	char fname[HTTP_FNAME_LEN];
	
	for (cmdp = http_static_files; cmdp->name != NULL; cmdp++)
		if (strcmp(cmdp->name, uri) == 0)
			break;
			
	if (cmdp->name == NULL) {
		hlog(LOG_DEBUG, "http static file request: 404: %s", uri);
		evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
		return;
	}
	
	snprintf(fname, HTTP_FNAME_LEN, "%s/%s", webdir, cmdp->filename);
	
	//hlog(LOG_DEBUG, "http static file request: %s", uri);
	
	return http_static_file(r, fname);
}

/*
 *	Return translated strings in JSON
 */

static void http_strings(struct evhttp_request *r, const char *uri)
{
	const char *query;
	
	query = evhttp_uri_get_query(evhttp_request_get_evhttp_uri(r));
	//hlog(LOG_DEBUG, "strings query: %s", query);
	
	const char *lang = NULL;
	struct evkeyvalq args;
	
	if (evhttp_parse_query_str(query, &args) == 0) {
		lang = evhttp_find_header(&args, "lang");
		//hlog(LOG_DEBUG, "lang: %s, checking against %d languages", lang, http_language_count);
		
		int i;
		for (i = 0; lang && i < http_language_count; i++) {
			if (strcasecmp(lang, http_language_files[i]->name) == 0) {
				hlog(LOG_DEBUG, "http strings query: %s: %s", lang, http_language_files[i]->filename);
				evhttp_clear_headers(&args);
				
				return http_static_file(r, http_language_files[i]->filename);
			}
		}
	}
	
	hlog(LOG_DEBUG, "http strings query: 404: %s", uri);
	evhttp_send_error(r, HTTP_NOTFOUND, "Not found");
	
	evhttp_clear_headers(&args);
	
	return;
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
		
		if (strncmp(uri, "/strings?", 9) == 0) {
			http_strings(r, uri);
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

/*
 *	The connection has been closed, or it is being rejected and closed.
 */

static void http_conn_closed(struct evhttp_connection *evcon, void *arg)
{
	struct http_conn_t *hc = arg;
	
	hfree(hc);
	http_clients--;
}

static void http_conn_attach_done(struct http_conn_t *hc)
{
	event_free(hc->attach_ev);
	hc->attach_ev = NULL;
	
	*hc->prevp = hc->next;
	if (hc->next)
		hc->next->prevp = hc->prevp;
}

/*
 *	Pick up a connection accepted on the previous round of the event
 *	loop, and if it is over the limit, close it. The server has by now
 *	set its callbacks on the bufferevent, with the connection as their
 *	argument. This runs at a higher priority than the I/O callbacks, so
 *	that the connection can not be closed and freed by one of them first.
 */

static void http_conn_attach(evutil_socket_t fd, short events, void *arg)
{
	struct http_conn_t *hc = arg;
	struct evhttp_connection *evcon = NULL;
	bufferevent_data_cb readcb = NULL;
	
	http_conn_attach_done(hc);
	
	bufferevent_getcb(hc->bev, &readcb, NULL, NULL, (void **)&evcon);
	if (!readcb || !evcon || evhttp_connection_get_bufferevent(evcon) != hc->bev) {
		hlog(LOG_ERR, "http: a new connection is not set up by the server as expected, not counting it");
		hfree(hc);
		http_clients--;
		return;
	}
	
	evhttp_connection_set_closecb(evcon, http_conn_closed, hc);
	
	if (http_clients > http_clients_max) {
		static time_t rejected_logged = 0;
		
		http_connections_rejected++;
		if (tick - rejected_logged > 60) {
			rejected_logged = tick;
			hlog(LOG_WARNING, "http: %d connections open, over the limit of %d: closing new connections (%lu closed so far)",
				http_clients, http_clients_max, http_connections_rejected);
		}
		
		/* closes the socket, and calls http_conn_closed */
		evhttp_connection_free(evcon);
	}
}

/*
 *	A connection has been accepted on an HTTP server: give it its
 *	bufferevent, count it, and have it picked up on the next round.
 *	Returning NULL would make the server use a bufferevent of its own, so
 *	an allocation failure leaves the connection uncounted.
 */

static struct bufferevent *http_conn_new(struct event_base *base, void *arg)
{
	struct timeval now_tv = { 0, 0 };
	struct bufferevent *bev;
	struct http_conn_t *hc;
	
	bev = bufferevent_socket_new(base, -1, 0);
	if (!bev) {
		hlog(LOG_ERR, "http: bufferevent_socket_new failed for a new connection");
		return NULL;
	}
	
	hc = hmalloc(sizeof(*hc));
	memset(hc, 0, sizeof(*hc));
	hc->bev = bev;
	
	hc->attach_ev = evtimer_new(base, http_conn_attach, hc);
	event_priority_set(hc->attach_ev, 0);
	evtimer_add(hc->attach_ev, &now_tv);
	
	hc->next = http_conn_attaching;
	hc->prevp = &http_conn_attaching;
	if (hc->next)
		hc->next->prevp = &hc->next;
	http_conn_attaching = hc;
	
	http_connections++;
	http_clients++;
	
	return bev;
}

/*
 *	The connection limit: as configured, or by default what is left of
 *	the file descriptors above MaxClients.
 */

static int http_clients_limit(void)
{
	int l, m;
	
	if (http_maxclients > 0)
		return http_maxclients;
	
	/* what is left above MaxClients and 200 spare descriptors, but at
	 * least a tenth of the descriptors, or 100, whichever is smaller */
	l = fileno_limit - maxclients - 200;
	m = fileno_limit / 10;
	if (m > 100)
		m = 100;
	if (l < m)
		l = m;
	if (l < 1)
		l = 1;
	
	return l;
}

/*
 *	Resume the listeners paused by an accept() failure a second ago
 */

static void http_listener_resume(evutil_socket_t fd, short events, void *arg)
{
	struct http_listener_t *l;
	
	hlog(LOG_DEBUG, "http: resuming the HTTP listeners");
	for (l = http_listeners; (l); l = l->next)
		evconnlistener_enable(l->listener);
}

/*
 *	accept() failed on an HTTP listener. The listener's read event is
 *	level-triggered, so when the process is out of file descriptors,
 *	the failure would repeat on every round of the event loop, spinning
 *	the thread. Pause the listeners for a second instead; the connections
 *	wait in the kernel's backlog meanwhile.
 */

static void http_listener_error(struct evconnlistener *lev, void *arg)
{
	static time_t error_logged = 0;
	int err = EVUTIL_SOCKET_ERROR();
	struct timeval resume_tv = { 1, 0 };
	struct http_listener_t *l;
	
	if (err != EMFILE && err != ENFILE) {
		if (tick - error_logged > 60) {
			error_logged = tick;
			hlog(LOG_ERR, "http: accept() failed: %s", strerror(err));
		}
		return;
	}
	
	if (tick - error_logged > 60) {
		error_logged = tick;
		hlog(LOG_ERR, "http: accept() failed: %s - pausing the HTTP listeners for a second", strerror(err));
	}
	
	for (l = http_listeners; (l); l = l->next)
		evconnlistener_disable(l->listener);
	
	if (!evtimer_pending(ev_listener_resume, NULL))
		evtimer_add(ev_listener_resume, &resume_tv);
}

static void http_srvr_defaults(struct evhttp *srvr)
{
	// limit what the clients can do a bit
	evhttp_set_allowed_methods(srvr, EVHTTP_REQ_GET);
	evhttp_set_timeout(srvr, 30);
	evhttp_set_max_body_size(srvr, 10*1024);
	evhttp_set_max_headers_size(srvr, 10*1024);
	
	// count the connections, and limit them
	evhttp_set_bevcb(srvr, http_conn_new, NULL);
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
	
	/* the listeners went with the servers */
	while (http_listeners) {
		struct http_listener_t *l = http_listeners;
		http_listeners = l->next;
		hfree(l);
	}
	
	if (ev_listener_resume) {
		event_free(ev_listener_resume);
		ev_listener_resume = NULL;
	}
	
	/* connections accepted on the last round, not picked up yet: the
	 * servers have closed them */
	while (http_conn_attaching) {
		struct http_conn_t *hc = http_conn_attaching;
		http_conn_attach_done(hc);
		hfree(hc);
		http_clients--;
	}
	
	if (libbase) {
		event_base_free(libbase);
		libbase = NULL;
	}
	
	if (http_language_files && http_language_count) {
		int i;
		for (i = 0; i < http_language_count; i++) {
			hfree(http_language_files[i]->name);
			hfree(http_language_files[i]->filename);
			hfree(http_language_files[i]);
			http_language_files[i] = NULL;
		}
		hfree(http_language_files);
		http_language_files = NULL;
		http_language_count = 0;
	}
}

/*
 *	Scan for language string files
 */

void lang_scan(void)
{
	hlog(LOG_DEBUG, "Scanning languages");
	
	struct http_static_t **new_language_files = NULL;
	int languages_loaded = 0;
	
#define LANG_FNAME_PREFIX "strings-"
#define LANG_FNAME_SUFFIX ".json"
	const char glob_s[] = LANG_FNAME_PREFIX "*" LANG_FNAME_SUFFIX;
	int glob_l = strlen(webdir) + 1 + strlen(glob_s) + 1;
	char *fullglob = hmalloc(glob_l);
	snprintf(fullglob, glob_l, "%s/%s", webdir, glob_s);
	
	glob_t globbuf;
	
	int ret = glob(fullglob, GLOB_NOSORT|GLOB_ERR, NULL, &globbuf);
	if (ret == 0) {
		int i;
		
		hlog(LOG_DEBUG, "%zu language files found", globbuf.gl_pathc);
		
		new_language_files = hmalloc(sizeof(*new_language_files) * globbuf.gl_pathc);
		memset(new_language_files, 0, sizeof(*new_language_files) * globbuf.gl_pathc);
		
		for (i = 0; i < globbuf.gl_pathc; i++) {
			hlog(LOG_DEBUG, "Language file: %s", globbuf.gl_pathv[i]);
			
			char *lang = NULL;
			char *bp, *ep = NULL;
			bp = strstr(globbuf.gl_pathv[i], LANG_FNAME_PREFIX);
			if (bp) {
				bp += strlen(LANG_FNAME_PREFIX);
				ep = strstr(bp, LANG_FNAME_SUFFIX);
			}
			
			if (ep) {
				int langlen = ep - bp;
				lang = hmalloc(langlen + 1);
				strncpy(lang, bp, langlen + 1); 
				lang[langlen] = 0;
				
				new_language_files[languages_loaded] = hmalloc(sizeof(struct http_static_t));
				new_language_files[languages_loaded]->name = lang;
				new_language_files[languages_loaded]->filename = hstrdup(globbuf.gl_pathv[i]);
				
				hlog(LOG_INFO, "Language %d installed: %s: %s", languages_loaded, lang, globbuf.gl_pathv[i]);
				
				languages_loaded++;
			}
			
		}
	} else {
		switch (ret) {
			case GLOB_NOSPACE:
				hlog(LOG_ERR, "Language file search failed: Out of memory");
				break;
			case GLOB_ABORTED:
				hlog(LOG_ERR, "Language file search failed: Read error / %s", strerror(errno));
				break;
			case GLOB_NOMATCH:
				hlog(LOG_INFO, "Language file search failed: No files found");
				break;
			default:
				break;
		}
	}
	
	globfree(&globbuf);
	hfree(fullglob);
	
	http_language_count = languages_loaded;
	http_language_files = new_language_files;
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

	thread_name_set("aprsc http");
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
			if (http_clients)
				hlog(LOG_ERR, "http: %d connections still counted after the servers were freed", http_clients);
			
			http_clients_max = http_clients_limit();
			hlog(LOG_INFO, "HTTP connection limit %d%s", http_clients_max, (http_maxclients > 0) ? "" : " (default)");
			
			lang_scan();
			
			// do init
#if 1
			libbase = event_base_new(); // libevent 2.x
#else
                        libbase = event_init(); // libevent 1.x
#endif
			// two priorities: 0 for picking up new connections, 1 for the rest
			event_base_priority_init(libbase, 2);
			
			// timer for the whole libevent, to catch shutdown signal
			ev_timer = event_new(libbase, -1, EV_TIMEOUT, http_timer, NULL);
			event_add(ev_timer, &http_timer_tv);
			
			// timer for resuming the listeners after an accept() failure
			ev_listener_resume = evtimer_new(libbase, http_listener_resume, NULL);
			
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
					continue;
				}
				
				struct http_listener_t *l = hmalloc(sizeof(*l));
				l->listener = evhttp_bound_socket_get_listener(handle);
				l->next = http_listeners;
				http_listeners = l;
				evconnlistener_set_error_cb(l->listener, http_listener_error);
			}
			
			hlog(LOG_INFO, "HTTP thread ready.");
			
			/* the http configuration list has been walked: the main
			 * thread may now read the next configuration */
			__sync_synchronize();
			http_config_applied = config_generation;
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

