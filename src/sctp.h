
#ifndef SCTP_H
#define SCTP_H

#ifdef USE_SCTP

#include "accept.h"

extern int sctp_set_client_sockopt(struct listen_t *l, struct client_t *c);
extern int sctp_set_listen_params(struct listen_t *l);
extern int sctp_readable(struct worker_t *self, struct client_t *c);
extern int sctp_writable(struct worker_t *self, struct client_t *c);
extern int sctp_client_write(struct worker_t *self, struct client_t *c, char *p, int len);
#endif

#endif
