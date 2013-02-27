
#ifdef HAVE_OPENSSL_SSL_H
#define USE_SSL
#endif

#ifndef SSL_H
#define SSL_H

#ifdef USE_SSL
extern int ssl_init(void);

#else

#define ssl_init(...) { }

#endif /* USE_SSL */
#endif /* SSL_H */

