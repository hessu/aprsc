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
	int	 alen;	// Address length
	int	 plen;	// Payload length
	char	 addresses[20];
	char	*packet;
	char	 packetbuf[200]; /* 99.9+ % of time this is enough.. */
};

#define DUPECHECK_CELL_SIZE sizeof(struct dupe_record_t)

extern long long dupecheck_outcount;  /* statistics counter */
extern long long dupecheck_dupecount; /* statistics counter */
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
