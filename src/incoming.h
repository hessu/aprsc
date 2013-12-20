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

extern const char *inerr_labels[];

extern int check_invalid_src_dst(const char *call, int len);
extern int check_call_match(const char **set, const char *call, int len);
extern int check_path_calls(const char *via_start, const char *path_end);

extern void incoming_flush(struct worker_t *self);
extern int incoming_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len);
extern int incoming_parse(struct worker_t *self, struct client_t *c, char *s, int len);

#ifndef _FOR_VALGRIND_
extern void incoming_cell_stats(struct cellstatus_t *cellst_pbuf_small,
	struct cellstatus_t *cellst_pbuf_medium,
	struct cellstatus_t *cellst_pbuf_large);
#endif

#endif

