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

/* error codes for incoming packet drop reasons */
#define INERR_NO_COLON			-1	/* no : in packet */
#define INERR_NO_DST			-2	/* no > in packet to mark end of dstcall */
#define INERR_NO_PATH			-3	/* no path found between srccall and : */
#define INERR_LONG_SRCCALL		-4	/* too long srccall */
#define INERR_NO_BODY			-5	/* no packet body/data after : */
#define INERR_LONG_DSTCALL		-6	/* too long dstcall */
#define INERR_DISALLOW_UNVERIFIED	-7	/* disallow_unverified = 1, unverified client */
#define INERR_NOGATE			-8	/* packet path has NOGATE/RFONLY */
#define INERR_3RD_PARTY			-9	/* 3rd-party packet dropped */
#define INERR_OUT_OF_PBUFS		-10	/* aprsc ran out of packet buffers */
#define INERR_CLASS_FAIL		-11	/* aprsc failed to classify packet */
#define INERR_Q_BUG			-12	/* aprsc q construct code bugging */
#define INERR_Q_DROP			-13	/* q construct drop */
#define INERR_MAX			-13	/* MAX VALUE FOR INERR */


extern char *memstr(char *needle, char *haystack, char *haystack_end);

extern void incoming_flush(struct worker_t *self);
extern int incoming_handler(struct worker_t *self, struct client_t *c, int l4proto, char *s, int len);
extern int incoming_parse(struct worker_t *self, struct client_t *c, char *s, int len);

#ifndef _FOR_VALGRIND_
extern void incoming_cell_stats(struct cellstatus_t *cellst_pbuf_small,
	struct cellstatus_t *cellst_pbuf_medium,
	struct cellstatus_t *cellst_pbuf_large);
#endif

#endif

