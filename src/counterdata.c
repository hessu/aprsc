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
#include "worker.h"
#include "hlog.h"

struct cdata_t {
	struct cdata_t *next;
	struct cdata_t **prevp;
	pthread_mutex_t mt;
	char *name;
	long long last_raw_value;
	time_t times[CDATA_SAMPLES];
	long long values[CDATA_SAMPLES];
	int last_index;
	int is_gauge;
};

struct cdata_t *counterdata = NULL;
pthread_mutex_t counterdata_mt = PTHREAD_MUTEX_INITIALIZER;

struct cdata_t *cdata_alloc(const char *name)
{
	int e;
	struct cdata_t *cd;
	
	cd = hmalloc(sizeof(*cd));
	memset(cd, 0, sizeof(*cd));
	
	pthread_mutex_init(&cd->mt, NULL);
	cd->name = hstrdup(name);
	cd->last_index = -1; // no data inserted yet
	cd->is_gauge = 0;
	
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
	
	//hlog(LOG_DEBUG, "cdata: allocated: %s", cd->name);
	
	return cd;
}

void cdata_free(struct cdata_t *cd)
{
	if (cd->next)
		cd->next->prevp = cd->prevp;
	*cd->prevp = cd->next;
	
	hfree(cd->name);
	hfree(cd);
}

static struct cdata_t *cdata_find_and_lock(const char *name)
{
	struct cdata_t *cd = NULL;
	int e;
	
	if ((e = pthread_mutex_lock(&counterdata_mt))) {
		hlog(LOG_CRIT, "cdata_find_and_lock: failed to lock counterdata_mt: %s", strerror(e));
		exit(1);
	}
	
	for (cd = counterdata; (cd); cd = cd->next)
		if (strcmp(name, cd->name) == 0) {
			if ((e = pthread_mutex_lock(&cd->mt))) {
				hlog(LOG_CRIT, "cdata_find_and_lock: could not lock cd: %s", strerror(e));
				exit(1);
			}
			break;
		}
	
	if ((e = pthread_mutex_unlock(&counterdata_mt))) {
		hlog(LOG_CRIT, "cdata_find_and_lock: could not unlock counterdata_mt: %s", strerror(e));
		exit(1);
	}
	
	return cd;
}

void cdata_counter_sample(struct cdata_t *cd, long long value)
{
	int e;
	long long l;
	
	if ((e = pthread_mutex_lock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_counter_sample %s: failed to lock mt: %s", cd->name, strerror(e));
		exit(1);
	}
	
	cd->last_index++;
	if (cd->last_index >= CDATA_SAMPLES)
		cd->last_index = 0;
	
	/* calculate counter's increment and insert */
	if (value == -1) {
		/* no data for sample */
		l = -1;
	} else {
		/* check for wrap-around */
		if (value < cd->last_raw_value)
			l = -1;
		else
			l = value - cd->last_raw_value;
	}
	
	cd->last_raw_value = value;
	cd->values[cd->last_index] = l;
	cd->times[cd->last_index] = tick;
	
	if ((e = pthread_mutex_unlock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_counter_sample %s: could not unlock counterdata_mt: %s", cd->name, strerror(e));
		exit(1);
	}
}

void cdata_gauge_sample(struct cdata_t *cd, long long value)
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
	cd->times[cd->last_index] = tick;
	cd->is_gauge = 1;
	
	if ((e = pthread_mutex_unlock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_gauge_sample %s: could not unlock counterdata_mt: %s", cd->name, strerror(e));
		exit(1);
	}
}

long cdata_get_last_value(const char *name)
{
	int e;
	long v;
	struct cdata_t *cd;
	
	cd = cdata_find_and_lock(name);
	
	if (!cd)
		return -1;
	
	if (cd->last_index < 0) {
		v = -1;
	} else {
		v = cd->values[cd->last_index];
	}
	
	if ((e = pthread_mutex_unlock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_get_last_value %s: could not unlock counterdata_mt: %s", cd->name, strerror(e));
		exit(1);
	}
	
	return v;
}

char *cdata_json_string(const char *name)
{
	struct cdata_t *cd;
	char *out = NULL;
	int i, e;
	
	cd = cdata_find_and_lock(name);
	
	if (!cd)
		return NULL;
	
	cJSON *root = cJSON_CreateObject();
	cJSON *values = cJSON_CreateArray();
	if (cd->is_gauge)
		cJSON_AddNumberToObject(root, "gauge", 1);
	cJSON_AddNumberToObject(root, "interval", CDATA_INTERVAL);
	cJSON_AddItemToObject(root, "values", values);
	
	if (cd->last_index >= 0) {
		i = cd->last_index + 1;
		do {
			if (i == CDATA_SAMPLES)
				i = 0;
			//hlog(LOG_DEBUG, "cdata_json_string, sample %d", i);
			
			if (cd->times[i] > 0) {
				cJSON *val = cJSON_CreateArray();
				cJSON_AddItemToArray(val, cJSON_CreateNumber(cd->times[i]));
				cJSON_AddItemToArray(val, cJSON_CreateNumber(cd->values[i]));
				cJSON_AddItemToArray(values, val);
			}
			
			if (i == cd->last_index)
				break;
			i ++;
		} while (1);
	}
	
	if ((e = pthread_mutex_unlock(&cd->mt))) {
		hlog(LOG_CRIT, "cdata_get_last_value %s: could not unlock counterdata_mt: %s", cd->name, strerror(e));
		exit(1);
	}
	
	out = cJSON_Print(root);
	cJSON_Delete(root);
	
	return out;
}

