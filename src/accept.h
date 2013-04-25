/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef ACCEPT_H
#define ACCEPT_H

#include "config.h"
#include "cJSON.h"

/*
 *	The listen_t structure holds data for a currently open
 *	listener. It's allocated when a listener is created
 *	based on the configuration (listen_config_t).
 */

struct listen_t {
	struct listen_t *next;
	struct listen_t **prevp;
	
	int id; /* random id */
	int listener_id; /* hash of protocol and local bound address */
	int fd;
	int client_flags;
	int portnum;
	int clients_max;
	int corepeer;
	int hidden;
	int ai_protocol;

	struct client_udp_t *udp;
	struct portaccount_t *portaccount;
	struct acl_t *acl;
#ifdef USE_SSL
	struct ssl_t *ssl;
#endif

	char *name;
	char *addr_s;
	char *filters[10]; // up to 10 filter definitions
};


extern int accept_reconfiguring;
extern int accept_shutting_down;

extern struct worker_t *udp_worker;

extern void accept_thread(void *asdf);

extern int accept_listener_status(cJSON *listeners, cJSON *totals);

extern int connections_accepted;

#endif
