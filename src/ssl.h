
#include "ac-hdrs.h"

#ifdef HAVE_OPENSSL_SSL_H
#define USE_SSL
#endif

#ifndef SSL_H
#define SSL_H

#ifdef USE_SSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <openssl/engine.h>
#include <openssl/evp.h>

struct client_t;
struct worker_t;

struct ssl_t {
	SSL_CTX *ctx;
};

struct ssl_connection_t {
	SSL             *connection;
/*
    ngx_int_t                   last;
    ngx_buf_t                  *buf;

    ngx_connection_handler_pt   handler;

    ngx_event_handler_pt        saved_read_handler;
    ngx_event_handler_pt        saved_write_handler;
*/
    unsigned                    handshaked:1;
    unsigned                    renegotiation:1;
    unsigned                    buffer:1;
    unsigned                    no_wait_shutdown:1;
    unsigned                    no_send_shutdown:1;
};

#define NGX_SSL_SSLv2    0x0002
#define NGX_SSL_SSLv3    0x0004
#define NGX_SSL_TLSv1    0x0008
#define NGX_SSL_TLSv1_1  0x0010
#define NGX_SSL_TLSv1_2  0x0020


#define NGX_SSL_BUFFER   1
#define NGX_SSL_CLIENT   2

#define NGX_SSL_BUFSIZE  16384

/* initialize the library */
extern int ssl_init(void);

/* per-listener structure allocators */
extern struct ssl_t *ssl_alloc(void);
extern void ssl_free(struct ssl_t *ssl);

/* create context for listener, load certs */
extern int ssl_create(struct ssl_t *ssl, void *data);
extern int ssl_certificate(struct ssl_t *ssl, const char *certfile, const char *keyfile);

/* create / free connection */
extern int ssl_create_connection(struct ssl_t *ssl, struct client_t *c, int i_am_client);

extern int ssl_write(struct worker_t *self, struct client_t *c);
extern int ssl_writeable(struct worker_t *self, struct client_t *c);
extern int ssl_readable(struct worker_t *self, struct client_t *c);


#else

struct ssl_t {
};


#define ssl_init(...) { }

#endif /* USE_SSL */
#endif /* SSL_H */

