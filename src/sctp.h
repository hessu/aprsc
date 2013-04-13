
#ifndef SCTP_H
#define SCTP_H

#ifdef USE_SCTP
extern int sctp_readable(struct worker_t *self, struct client_t *c);
extern int sctp_writable(struct worker_t *self, struct client_t *c);
extern int sctp_client_write(struct worker_t *self, struct client_t *c, char *p, int len);
#endif

#endif
