/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef INCOMING_H
#define INCOMING_H

#include "worker.h"
#include "cellmalloc.h"

extern char *memstr(char *needle, char *haystack, char *haystack_end);

extern void incoming_flush(struct worker_t *self);
extern int incoming_handler(struct worker_t *self, struct client_t *c, char *s, int len);
extern int incoming_uplinksim_handler(struct worker_t *self, struct client_t *c, char *s, int len);

extern void incoming_cell_stats(struct cellstatus_t *cellst_pbuf_small,
	struct cellstatus_t *cellst_pbuf_medium,
	struct cellstatus_t *cellst_pbuf_large);

#endif

