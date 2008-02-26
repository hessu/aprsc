
#ifndef INCOMING_H
#define INCOMING_H

#include "worker.h"

extern void incoming_flush(struct worker_t *self);
extern int incoming_handler(struct worker_t *self, struct client_t *c, char *s, int len);

#endif

