/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef DUPECHECK_H
#define DUPECHECK_H

#include "worker.h"
#include "cellmalloc.h"

struct dupe_record_t {
	struct dupe_record_t *next;
	uint32_t hash;
	time_t	 t;
	int	 dtype; // dupecheck dupe type
	int	 len;	// address + payload length
	char	*packet;
	char	 packetbuf[220]; /* 99.9+ % of time this is enough.. */
};

#define DUPECHECK_CELL_SIZE sizeof(struct dupe_record_t)

#define DTYPE_SPACE_TRIM	1
#define DTYPE_STRIP_8BIT	2
#define DTYPE_CLEAR_8BIT	3
#define DTYPE_SPACED_8BIT	4
#define DTYPE_LOWDATA_STRIP	5
#define DTYPE_LOWDATA_SPACED	6
#define DTYPE_DEL_STRIP		7
#define DTYPE_DEL_SPACED	8
#define DTYPE_MAX		8

extern long long dupecheck_outcount;  /* statistics counter */
extern long long dupecheck_dupecount; /* statistics counter */
extern long long dupecheck_dupetypes[DTYPE_MAX+1];
extern long      dupecheck_cellgauge; /* statistics gauge   */

extern int  outgoing_lag_report(struct worker_t *self, int*lag, int*dupelag);

extern void dupecheck_init(void);
extern void dupecheck_start(void);
extern void dupecheck_stop(void);
extern void dupecheck_atend(void);

/* cellmalloc status */
#ifndef _FOR_VALGRIND_
extern void dupecheck_cell_stats(struct cellstatus_t *cellst);
#endif

#endif
