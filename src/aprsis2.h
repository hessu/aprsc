
#ifndef APRSIS2_H
#define APRSIS2_H

#include "worker.h"

extern int is2_out_server_signature(struct worker_t *self, struct client_t *c);
extern int is2_deframe_input(struct worker_t *self, struct client_t *c, int start_at);

extern int is2_out_ping(struct worker_t *self, struct client_t *c);

#endif

