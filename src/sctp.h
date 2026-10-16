
#ifndef SCTP_H
#define SCTP_H

#ifdef USE_SCTP

#include "accept.h"

extern int sctp_set_client_sockopt(struct client_t *c);
extern int sctp_set_client_sockopt_post_login(struct client_t *c);
extern int sctp_wait_sender_dry(struct client_t *c);
extern void sctp_dry_wait_expire(struct worker_t *self, struct client_t *c);
extern int sctp_set_listen_params(struct listen_t *l);
extern int sctp_bindx_add(int fd, const struct sockaddr *sa, socklen_t len);
extern int sctp_socket_multihome(int family, const struct sockaddr *const *addrs, const socklen_t *lens, int n, int reuseport, const char *kind, const char *name);
extern int sctp_readable(struct worker_t *self, struct client_t *c);
extern int sctp_writable(struct worker_t *self, struct client_t *c);
extern int sctp_client_write(struct worker_t *self, struct client_t *c, char *p, int len);
#endif

#endif
