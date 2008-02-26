
#ifndef LOGIN_H
#define LOGIN_H

#include "worker.h"

extern int login_handler(struct worker_t *self, struct client_t *c, char *s, int len);

#endif

