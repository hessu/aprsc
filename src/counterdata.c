/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 *
 *	The counterdata module stores periodic samples of counter values
 *	to facilitate calculating average traffic levels and do some
 *	fancy bells and whistles on the status page.
 */

#include "ac-hdrs.h"

#include <pthread.h>
#include <string.h>
#include <errno.h>

#include "counterdata.h"
#include "config.h"
#include "hmalloc.h"
#include "hlog.h"

struct cdata_t {
	struct cdata_t *next;
	struct cdata_t **prevp;
	pthread_mutex_t mt;
	char *name;
	long last_raw_value;
	long values[CDATA_SAMPLES];
	int last_index;
};

struct cdata_t *counterdata = NULL;
pthread_mutex_t counterdata_mt = PTHREAD_MUTEX_INITIALIZER;

struct cdata_t *cdata_allocate(const char *name)
{
	int e;
	struct cdata_t *cd;
	
	cd = hmalloc(sizeof(*cd));
	memset(cd, 0, sizeof(*cd));
	
	pthread_mutex_init(&cd->mt, NULL);
	cd->name = hstrdup(name);
	cd->last_index = -1; // no data inserted yet
	
	if ((e = pthread_mutex_lock(&counterdata_mt))) {
		hlog(LOG_CRIT, "cdata_allocate: failed to lock counterdata_mt: %s", strerror(e));
		exit(1);
	}
	
	cd->next = counterdata;
	if (counterdata)
		counterdata->prevp = &cd->next;
	counterdata = cd;
	cd->prevp = &counterdata;
	
	if ((e = pthread_mutex_unlock(&counterdata_mt))) {
		hlog(LOG_CRIT, "cdata_allocate: could not unlock counterdata_mt: %s", strerror(e));
		exit(1);
	}
	
	return cd;
}

void cdata_counter_sample(struct cdata_t *cd, long value)
{
	int e;
	long l;
	
	if ((e = pthread_mutex_lock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_counter_sample %s: failed to lock mt: %s", cd->name, strerror(e));
		exit(1);
	}
	
	cd->last_index++;
	if (cd->last_index >= CDATA_SAMPLES)
		cd->last_index = 0;
	
	/* calculate counter's increment and insert */
	l = value - cd->last_raw_value;
	cd->last_raw_value = value;
	cd->values[cd->last_index] = l;
	
	if ((e = pthread_mutex_unlock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_counter_sample %s: could not unlock counterdata_mt: %s", cd->name, strerror(e));
		exit(1);
	}
}

void cdata_gauge_sample(struct cdata_t *cd, long value)
{
	int e;
	
	if ((e = pthread_mutex_lock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_gauge_sample %s: failed to lock mt: %s", cd->name, strerror(e));
		exit(1);
	}
	
	cd->last_index++;
	if (cd->last_index >= CDATA_SAMPLES)
		cd->last_index = 0;
	
	/* just insert the gauge */
	cd->last_raw_value = value;
	cd->values[cd->last_index] = value;
	
	if ((e = pthread_mutex_unlock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_gauge_sample %s: could not unlock counterdata_mt: %s", cd->name, strerror(e));
		exit(1);
	}
}

