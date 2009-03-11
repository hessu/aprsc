/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */
#ifndef CLIENTLIST_H
#define CLIENTLIST_H

#include "worker.h"

extern void clientlist_add(struct client_t *c);
extern void clientlist_remove(struct client_t *c);

extern int clientlist_check_if_validated_client(char *username, int len);

#endif

