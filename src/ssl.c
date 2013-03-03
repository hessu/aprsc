
/*
 *	This OpenSSL interface code has been proudly copied from
 *	the excellent NGINX web server.
 *
 *	Its license is reproduced here.
 */

/* 
 * Copyright (C) 2002-2013 Igor Sysoev
 * Copyright (C) 2011-2013 Nginx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"
#include "ssl.h"
#include "hlog.h"
#include "hmalloc.h"
#include "worker.h"

#ifdef USE_SSL

#include <openssl/conf.h>
#include <openssl/ssl.h>

#define NGX_DEFAULT_CIPHERS     "HIGH:!aNULL:!MD5"
#define SSL_PROTOCOLS (NGX_SSL_SSLv3|NGX_SSL_TLSv1 |NGX_SSL_TLSv1_1|NGX_SSL_TLSv1_2)
int  ssl_available;
int  ssl_connection_index;
int  ssl_server_conf_index;
int  ssl_session_cache_index;

int ssl_init(void)
{
	hlog(LOG_INFO, "Initializing OpenSSL...");
	
	OPENSSL_config(NULL);
	
	SSL_library_init();
	SSL_load_error_strings();
	
	OpenSSL_add_all_algorithms();
	
#if OPENSSL_VERSION_NUMBER >= 0x0090800fL
#ifndef SSL_OP_NO_COMPRESSION
	{
	/*
	 * Disable gzip compression in OpenSSL prior to 1.0.0 version,
	 * this saves about 522K per connection.
	 */
	int                  n;
	STACK_OF(SSL_COMP)  *ssl_comp_methods;
	
	ssl_comp_methods = SSL_COMP_get_compression_methods();
	n = sk_SSL_COMP_num(ssl_comp_methods);
	
	while (n--) {
		(void) sk_SSL_COMP_pop(ssl_comp_methods);
	}
	
	}
#endif
#endif

	ssl_connection_index = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	
	if (ssl_connection_index == -1) {
		hlog(LOG_ERR, "SSL_get_ex_new_index() failed");
		return -1;
	}
	
	ssl_server_conf_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	
	if (ssl_server_conf_index == -1) {
		hlog(LOG_ERR, "SSL_CTX_get_ex_new_index() failed");
		return -1;
	}
	
	ssl_session_cache_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	
	if (ssl_session_cache_index == -1) {
		hlog(LOG_ERR, "SSL_CTX_get_ex_new_index() failed");
		return -1;
	}
	
	ssl_available = 1;
	
	return 0;
}

struct ssl_t *ssl_alloc(void)
{
	struct ssl_t *ssl;
	
	ssl = hmalloc(sizeof(*ssl));
	memset(ssl, 0, sizeof(*ssl));
	
	return ssl;
}

void ssl_free(struct ssl_t *ssl)
{
	if (ssl->ctx)
	    SSL_CTX_free(ssl->ctx);
	    
	hfree(ssl);
}

int ssl_create(struct ssl_t *ssl, void *data)
{
	ssl->ctx = SSL_CTX_new(SSLv23_method());
	
	if (ssl->ctx == NULL) {
		hlog(LOG_ERR, "SSL_CTX_new() failed");
		return -1;
	}
	
	if (SSL_CTX_set_ex_data(ssl->ctx, ssl_server_conf_index, data) == 0) {
		hlog(LOG_ERR, "SSL_CTX_set_ex_data() failed");
		return -1;
	}
	
	/* client side options */
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_MICROSOFT_SESS_ID_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_NETSCAPE_CHALLENGE_BUG);
	
	/* server side options */
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_MICROSOFT_BIG_SSLV3_BUFFER);
	
	/* this option allow a potential SSL 2.0 rollback (CAN-2005-2969) */
	SSL_CTX_set_options(ssl->ctx, SSL_OP_MSIE_SSLV2_RSA_PADDING);
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_TLS_D5_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_TLS_BLOCK_PADDING_BUG);
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_SINGLE_DH_USE);
	
	/* SSL protocols not configurable for now */
	int protocols = SSL_PROTOCOLS;
	
	if (!(protocols & NGX_SSL_SSLv2)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_SSLv2);
	}
	if (!(protocols & NGX_SSL_SSLv3)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_SSLv3);
	}
	if (!(protocols & NGX_SSL_TLSv1)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_TLSv1);
	}
	
#ifdef SSL_OP_NO_TLSv1_1
	if (!(protocols & NGX_SSL_TLSv1_1)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_TLSv1_1);
	}
#endif
#ifdef SSL_OP_NO_TLSv1_2
	if (!(protocols & NGX_SSL_TLSv1_2)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_TLSv1_2);
	}
#endif

#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_COMPRESSION);
#endif

#ifdef SSL_MODE_RELEASE_BUFFERS
	SSL_CTX_set_mode(ssl->ctx, SSL_MODE_RELEASE_BUFFERS);
#endif
	
	SSL_CTX_set_read_ahead(ssl->ctx, 1);
	
	//SSL_CTX_set_info_callback(ssl->ctx, ngx_ssl_info_callback);
	
	return 0;
}

/*
 *	Load server key and certificate
 */

int ssl_certificate(struct ssl_t *ssl, const char *certfile, const char *keyfile)
{
	if (SSL_CTX_use_certificate_chain_file(ssl->ctx, certfile) == 0) {
		hlog(LOG_ERR, "SSL_CTX_use_certificate_chain_file(\"%s\") failed", certfile);
		return -1;
	}
	
	
	if (SSL_CTX_use_PrivateKey_file(ssl->ctx, keyfile, SSL_FILETYPE_PEM) == 0) {
		hlog(LOG_ERR, "SSL_CTX_use_PrivateKey_file(\"%s\") failed", keyfile);
		return -1;
	}
	
	return 0;
}

/*
 *	Create a connect */

int ssl_create_connection(struct ssl_t *ssl, struct client_t *c, int i_am_client)
{
	struct ssl_connection_t  *sc;
	
	sc = hmalloc(sizeof(*sc));
	
	//sc->buffer = ((flags & NGX_SSL_BUFFER) != 0);
	sc->connection = SSL_new(ssl->ctx);
	
	if (sc->connection == NULL) {
		hlog(LOG_ERR, "SSL_new() failed");
		return -1;
	}
	
	if (SSL_set_fd(sc->connection, c->fd) == 0) {
		hlog(LOG_ERR, "SSL_set_fd() failed");
		return -1;
	}
	
	if (i_am_client) {
		SSL_set_connect_state(sc->connection);
	} else {
		SSL_set_accept_state(sc->connection);
	}
	
	if (SSL_set_ex_data(sc->connection, ssl_connection_index, c) == 0) {
		hlog(LOG_ERR, "SSL_set_ex_data() failed");
		return -1;
	}
	
	c->ssl_con = sc;
	
	return 0;
}


int ssl_write(struct worker_t *self, struct client_t *c)
{
	int n;
	int sslerr;
	int err;
	
	hlog(LOG_DEBUG, "ssl_write");
	
	n = SSL_write(c->ssl_con->connection, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
	
	hlog(LOG_DEBUG, "SSL_write returned %d", n);
	
	if (n > 0) {
		/* ok, we wrote some */
		c->obuf_start += n;
		c->obuf_wtime = tick;
		
		/* All done ? */
		if (c->obuf_start >= c->obuf_end) {
			hlog(LOG_DEBUG, "ssl_write(%s) obuf empty", c->addr_rem);
			c->obuf_start = 0;
			c->obuf_end   = 0;
			return n;
		}
		
		/* tell the poller that we have outgoing data */
		xpoll_outgoing(&self->xp, c->xfd, 1);
		
		return n;
	}
	
	sslerr = SSL_get_error(c->ssl_con->connection, n);
	err = (sslerr == SSL_ERROR_SYSCALL) ? errno : 0;
	
	hlog(LOG_DEBUG, "ssl_write SSL_get_error: %d", sslerr);
	
	if (sslerr == SSL_ERROR_WANT_WRITE) {
		hlog(LOG_INFO, "ssl_write: says SSL_ERROR_WANT_WRITE, marking socket for write events");
		
		/* tell the poller that we have outgoing data */
		xpoll_outgoing(&self->xp, c->xfd, 1);
		
		return 0;
	}
	
	if (sslerr == SSL_ERROR_WANT_READ) {
		hlog(LOG_INFO, "ssl_write: says SSL_ERROR_WANT_READ, calling ssl_readable (peer started SSL renegotiation?)");
		
		return ssl_readable(self, c);
	}
	
	c->ssl_con->no_wait_shutdown = 1;
	c->ssl_con->no_send_shutdown = 1;
	
	hlog(LOG_DEBUG, "ssl_write: SSL_write() failed");
	client_close(self, c, errno);
	
	return -1;
}

int ssl_writeable(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_DEBUG, "ssl_writeable");
	
	return ssl_write(self, c);
}

int ssl_readable(struct worker_t *self, struct client_t *c)
{
	int r;
	int sslerr, err;
	
	hlog(LOG_DEBUG, "ssl_readable");
	
	r = SSL_read(c->ssl_con->connection, c->ibuf + c->ibuf_end, c->ibuf_size - c->ibuf_end - 1);
	
	if (r > 0) {
		/* we got some data... process */
		hlog(LOG_DEBUG, "SSL_read returned %d bytes of data", r);
		
		/* TODO: whatever the client_readable does */
		return client_postread(self, c, r);
	}
	
	sslerr = SSL_get_error(c->ssl_con->connection, r);
	err = (sslerr == SSL_ERROR_SYSCALL) ? errno : 0;
	
	hlog(LOG_DEBUG, "ssl_readable: SSL_get_error: %d", sslerr);
	
	if (sslerr == SSL_ERROR_WANT_READ) {
		hlog(LOG_DEBUG, "ssl_readable: SSL_read says SSL_ERROR_WANT_READ, doing it later");
		return 0;
	}
	
	if (sslerr == SSL_ERROR_WANT_WRITE) {
		hlog(LOG_INFO, "ssl_readable: SSL_read says SSL_ERROR_WANT_WRITE (peer starts SSL renegotiation?), calling ssl_writeable");
		return ssl_writeable(self, c);
	}
	
	c->ssl_con->no_wait_shutdown = 1;
	c->ssl_con->no_send_shutdown = 1;
	
	if (sslerr == SSL_ERROR_ZERO_RETURN || ERR_peek_error() == 0) {
		hlog(LOG_DEBUG, "ssl_readable: peer shutdown SSL cleanly");
		client_close(self, c, CLIERR_EOF);
		return -1;
	}
	
	hlog(LOG_DEBUG, "ssl_readable: SSL_read() failed");
	client_close(self, c, errno);
	return -1;
}


#endif

