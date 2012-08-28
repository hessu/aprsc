/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef LOGIN_H
#define LOGIN_H

#include "worker.h"

extern int http_udp_upload_login(const char *addr_rem, char *s, char **username);
extern int login_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len);

#endif

