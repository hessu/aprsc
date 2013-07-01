/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef UPLINK_H
#define UPLINK_H 1

#include "worker.h"

extern int uplink_reconfiguring;
extern int uplink_shutting_down;

extern void uplink_thread(void *asdf);
extern void uplink_close(struct client_t *c, int errnum);
extern void uplink_start(void);
extern void uplink_stop(void);
extern int  uplink_login_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len);

extern int uplink_server_validate_cert(struct worker_t *self, struct client_t *c);
extern int uplink_server_validate_cert_cn(struct worker_t *self, struct client_t *c);


#endif /* FILTER_H */
