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

extern int http_udp_upload_login(const char *addr_rem, char *s, char **username, const char *log_source);
extern int login_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len);
extern void login_set_app_name(struct client_t *c, const char *app_name, const char *app_ver);
extern int login_setup_udp_feed(struct client_t *c, int port);

#endif

