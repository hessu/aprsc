
#ifndef APRSIS2_H
#define APRSIS2_H

#include "worker.h"

extern void is2_allocate_obuf(struct client_t *c);

extern int is2_input_handler_login(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);
extern int is2_input_handler_uplink_wait_signature(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);
extern int is2_input_handler(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);

extern int is2_out_server_signature(struct worker_t *self, struct client_t *c);
extern int is2_deframe_input(struct worker_t *self, struct client_t *c, int start_at);
extern int is2_corepeer_deframe_input(struct worker_t *self, struct client_t *c, char *ibuf, int len);

extern int is2_write_packet(struct worker_t *self, struct client_t *c, char *p, int len);
extern int is2_obuf_flush(struct worker_t *self, struct client_t *c);
extern int is2_corepeer_obuf_flush(struct worker_t *self, struct client_t *c);

extern int is2_out_ping(struct worker_t *self, struct client_t *c);

extern int is2_corepeer_propose(struct worker_t *self, struct client_t *c);
extern int is2_corepeer_control_in(struct worker_t *self, struct client_t *c, char *p, int len);

#endif

