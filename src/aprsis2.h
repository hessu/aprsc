
#ifndef APRSIS2_H
#define APRSIS2_H

#include "worker.h"

extern int is2_out_server_signature(struct worker_t *self, struct client_t *c);
extern int is2_in_server_signature(struct worker_t *self, struct client_t *c, char *s, int len);
extern int is2_deframe_input(struct worker_t *self, struct client_t *c, int start_at);

#endif

