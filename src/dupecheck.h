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

extern long long dupecheck_outcount;  /* statistics counter */
extern long long dupecheck_dupecount; /* statistics counter */
extern int       dupecheck_cellgauge; /* statistics gauge   */

extern int  outgoing_lag_report(struct worker_t *self, int*lag, int*dupelag);

extern void dupecheck_init(void);
extern void dupecheck_start(void);
extern void dupecheck_stop(void);
extern void dupecheck_atend(void);

#endif
