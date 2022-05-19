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

static void http_send_reply_code(struct evhttp_request *r, struct evkeyvalq *headers, char *data, int len, int allow_compress, int http_code, char *http_code_text);

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

	/* prepare default response headers */
	http_header_base(evhttp_request_get_output_headers(r), 0);

	req_headers = evhttp_request_get_input_headers(r);
	ctype = evhttp_find_header(req_headers, "Content-Type");
	
	if (!ctype || strcasecmp(ctype, "application/octet-stream") != 0) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, wrong or missing content-type", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	clength = evhttp_find_header(req_headers, "Content-Length");
	if (!clength) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, missing content-length", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	clength_i = atoi(clength);
	if (clength_i > MAX_HTTP_POST_DATA) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, too large body", 0, 1, HTTP_BADREQUEST, NULL);
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
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, body size does not match content-length", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	hlog(LOG_DEBUG, "got post data: %s", post);
	
	packet_len = loginpost_split(post, l, &login_string, &packet);
	if (packet_len == -1) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, No newline (LF) found in data", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	if (!login_string) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, No login string in data", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	if (!packet) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, no packet data found in data", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	hlog(LOG_DEBUG, "login string: %s", login_string);
	hlog(LOG_DEBUG, "packet: %s", packet);
	
	/* process the login string */
	validated = http_udp_upload_login(remote_host, login_string, &username, "HTTP POST");
	if (validated < 0) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, invalid login string", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	if (validated != 1) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Invalid passcode", 0, 1, 403, NULL);
		return;
	}
	
	/* packet size limits */
	if (packet_len < PACKETLEN_MIN) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, packet too short", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	if (packet_len > PACKETLEN_MAX-2) {
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, packet too long", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	e = pseudoclient_push_packet(http_worker, http_pseudoclient, username, packet, packet_len);

	if (e < 0) {
		hlog(LOG_DEBUG, "http incoming packet parse failure code %d: %s", e, packet);
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, packet parsing failure", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	bufout = evbuffer_new();
	evbuffer_add(bufout, "ok\n", 3);
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
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
 *	Return correct HTTP description for the code
 */

static char *http_get_code_text(int http_code)
{
	switch (http_code) {
		case HTTP_OK:			/* 200 */ return "OK";
		case HTTP_NOCONTENT:		/* 204 */ return "No content";
		case HTTP_MOVEPERM:		/* 301 */ return "Moved permanently";
		case HTTP_MOVETEMP:		/* 302 */ return "Found";
		case HTTP_NOTMODIFIED:		/* 304 */ return "Not modified";
		case HTTP_BADREQUEST:		/* 400 */ return "Bad request";
		case 401:			/* 401 */ return "Unauthorized";
		case 403:			/* 403 */ return "Forbidden";
		case HTTP_NOTFOUND:		/* 404 */ return "Not found";
		case HTTP_BADMETHOD:		/* 405 */ return "Method not allowed";
		case HTTP_ENTITYTOOLARGE:	/* 413 */ return "Payload too large";
		case HTTP_EXPECTATIONFAILED:	/* 417 */ return "Expectation failed";
		case HTTP_INTERNAL:		/* 500 */ return "Internal server error";
		case HTTP_NOTIMPLEMENTED:	/* 501 */ return "Not implemented";
		case HTTP_SERVUNAVAIL:		/* 503 */ return "Service unavailable";
		default:			return "Unknown";
	};
}


/*
 *	Construct an error page - may be templated later to look nicer
 */

static char *http_construct_error_page(char *data, int http_code, char *http_code_text)
{
	char buffer[1024];
	char *s = NULL;

	snprintf(buffer, sizeof(buffer), "<!doctype html public \"-//IETF//DTD HTML 2.0//EN\">\n" \
		"<html><head>\n" \
		"<title>%d %s</title>\n" \
		"</head><body>\n" \
		"<h1>%s</h1>\n" \
		"<p>%s</p>\n" \
		"<hr>\n" \
		"</body></html>\n",
		http_code, (http_code_text ? http_code_text : http_get_code_text(http_code)),
		(http_code_text ? http_code_text : http_get_code_text(http_code)),
		data);
	s = hmalloc(strlen(buffer));
	snprintf(s, strlen(buffer), "%s", buffer);
	return s;
}


/*
 *	Transmit a HTTP response, given headers and data.
 *	May be OK or an error.
 *	Compress response, if possible.
 */

static void http_send_reply_code(struct evhttp_request *r, struct evkeyvalq *headers, char *data, int len, int allow_compress, int http_code, char *http_code_text)
{
	/* allow len=0 to be length of text (used for error codes) */
	if (data && len == 0)
		len = strlen(data);
	if (http_code != HTTP_OK)
	{
		/* construct an error page */
		data = http_construct_error_page(data, http_code, http_code_text);
		if (data)
			len = strlen(data);
	}
#ifdef HAVE_LIBZ
	char *compr = NULL;
	
	/* Gzipping files below 150 bytes can actually make them larger. */
	if (len > 150 && allow_compress) {
		/* Consider returning a compressed version */
		int compr_type = http_check_req_compressed(r);
		/*
		if (compr_type)
			hlog(LOG_DEBUG, "http_send_reply_code, client supports transfer-encoding: %s", compr_type_strings[compr_type]);
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
	
	evhttp_send_reply(r, http_code, (http_code_text ? http_code_text : http_get_code_text(http_code)), buffer);
	evbuffer_free(buffer);

#ifdef HAVE_LIBZ
	if (compr)
		hfree(compr);
#endif
	/* if we constructed an error page, free the memory */
	if (data && http_code != HTTP_OK)
		hfree(data);
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
	http_send_reply_code(r, headers, json, strlen(json), 1, HTTP_OK, "OK");
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
		http_header_base(evhttp_request_get_output_headers(r), 0);
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Bad request, no such counter", 0, 1, HTTP_BADREQUEST, NULL);
		return;
	}
	
	struct evkeyvalq *headers = evhttp_request_get_output_headers(r);
	http_header_base(headers, tick);
	evhttp_add_header(headers, "Content-Type", "application/json; charset=UTF-8");
	evhttp_add_header(headers, "Cache-Control", "max-age=58");
	
	http_send_reply_code(r, headers, json, strlen(json), 1, HTTP_OK, "OK");
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
		http_header_base(evhttp_request_get_output_headers(r), 0);
		if (errno == ENOENT) {
			/* don't complain about missing motd.html - it's optional. */
			int level = LOG_ERR;
			if (strcmp(fname, "motd.html") == 0)
				level = LOG_DEBUG;
			hlog(level, "http static file '%s' not found: 404", fname);
			http_send_reply_code(r, evhttp_request_get_output_headers(r), "Not found", 0, 1, HTTP_NOTFOUND, NULL);
			return;
		}
		
		hlog(LOG_ERR, "http static file '%s' could not be opened for reading: %s", fname, strerror(errno));
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Could not access file", 0, 1, HTTP_INTERNAL, NULL);
		return;
	}
	
	if (fstat(fd, &st) == -1) {
		hlog(LOG_ERR, "http static file '%s' could not fstat() after opening: %s", fname, strerror(errno));
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Could not access file", 0, 1, HTTP_INTERNAL, NULL);
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
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Could not access file", 0, 1, HTTP_INTERNAL, NULL);
		hfree(buf);
		return;
	}
	
	if (n != file_size) {
		hlog(LOG_ERR, "http static file '%s' could only read %d of %d bytes", fname, n, file_size);
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Could not access file", 0, 1, HTTP_INTERNAL, NULL);
		hfree(buf);
		return;
	}
	
	int allow_compress;
	if (strncmp(contenttype, "image/", 6) == 0)
		allow_compress = 0;
	else
		allow_compress = 1;
	
	http_send_reply_code(r, headers, buf, n, allow_compress, HTTP_OK, "OK");
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
		http_header_base(evhttp_request_get_output_headers(r), 0);
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Not found", 0, 1, HTTP_NOTFOUND, NULL);
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
		for (i = 0; i < http_language_count; i++) {
			if (strcasecmp(lang, http_language_files[i]->name) == 0) {
				hlog(LOG_DEBUG, "http strings query: %s: %s", lang, http_language_files[i]->filename);
				evhttp_clear_headers(&args);
				
				return http_static_file(r, http_language_files[i]->filename);
			}
		}
	}
	
	hlog(LOG_DEBUG, "http strings query: 404: %s", uri);
	http_header_base(evhttp_request_get_output_headers(r), 0);
	http_send_reply_code(r, evhttp_request_get_output_headers(r), "Not found", 0, 1, HTTP_NOTFOUND, NULL);
	
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
		http_header_base(evhttp_request_get_output_headers(r), 0);
		http_send_reply_code(r, evhttp_request_get_output_headers(r), "Not found", 0, 1, HTTP_NOTFOUND, NULL);
		return;
	}
	
	hlog(LOG_ERR, "http request on unknown server for '%s': 404 not found", uri);
	http_header_base(evhttp_request_get_output_headers(r), 0);
	http_send_reply_code(r, evhttp_request_get_output_headers(r), "Server not found", 0, 1, HTTP_NOTFOUND, NULL);
	
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
		
		hlog(LOG_DEBUG, "%d language files found", globbuf.gl_pathc);
		
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
			
			lang_scan();
			
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

