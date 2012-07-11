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

#include "cJSON.h"

extern int accept_reconfiguring;
extern int accept_shutting_down;

extern void accept_thread(void *asdf);

extern int accept_listener_status(cJSON *listeners, cJSON *totals);

extern int connections_accepted;

#endif
