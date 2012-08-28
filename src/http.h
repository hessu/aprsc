/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef HTTP_H
#define HTTP_H

#include "worker.h"

extern struct worker_t *http_worker;

extern int http_reconfiguring;
extern int http_shutting_down;

extern int loginpost_split(char *post, int len, char **login_string, char **packet);
extern int pseudoclient_push_packet(struct worker_t *worker, struct client_t *pseudoclient, const char *username, char *packet, int packet_len);

extern void http_thread(void *asdf);

#endif
