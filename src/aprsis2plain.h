
#ifndef APRSIS2PLAIN_H
#define APRSIS2PLAIN_H

#include "worker.h"

extern void is2_plain_metadata_reset(struct client_t *c);
extern int is2_plain_metadata(struct worker_t *self, struct client_t *c, const char *s, int len);

#endif
