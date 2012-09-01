/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef FILTER_H
#define FILTER_H 1

#include "worker.h"
#include "cellmalloc.h"

extern void filter_init(void);
extern int  filter_parse(struct client_t *c, const char *filt, int is_user_filter);
extern void filter_free(struct filter_t *c);
extern int  filter_process(struct worker_t *self, struct client_t *c, struct pbuf_t *pb);
extern int  filter_commands(struct worker_t *self, struct client_t *c, int in_message, const char *s, const int len);

extern void filter_preprocess_dupefilter(struct pbuf_t *pb);
extern void filter_postprocess_dupefilter(struct pbuf_t *pb);

extern void filter_entrycall_cleanup(void);
extern void filter_entrycall_atend(void);
extern int  filter_entrycall_cellgauge;
extern void filter_entrycall_dump(FILE *fp);

extern void filter_wx_cleanup(void);
extern void filter_wx_atend(void);
extern int  filter_wx_cellgauge;
extern void filter_wx_dump(FILE *fp);

extern int  filter_cellgauge;

extern float filter_lat2rad(float lat);
extern float filter_lon2rad(float lon);

#ifndef _FOR_VALGRIND_
extern void filter_cell_stats(struct cellstatus_t *filter_cellst,
	struct cellstatus_t *filter_entrycall_cellst,
	struct cellstatus_t *filter_wx_cellst);
#endif

#endif /* FILTER_H */
