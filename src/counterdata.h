/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef COUNTERDATA_H
#define COUNTERDATA_H

/* store 48 hours, 1 sample per minute */
#define CDATA_SAMPLES	48*60
#define CDATA_INTERVAL	60

struct cdata_t;

extern struct cdata_t *cdata_alloc(const char *name);
extern void cdata_counter_sample(struct cdata_t *cd, long value);
extern void cdata_gauge_sample(struct cdata_t *cd, long value);
extern long cdata_get_last_value(struct cdata_t *cd);

#endif