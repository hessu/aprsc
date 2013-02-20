/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef STATUS_H
#define STATUS_H

#include <time.h>
#include "cJSON.h"

extern time_t startup_tick, startup_time;
extern cJSON *liveupgrade_status;

extern void status_error(int ttl, const char *err);

extern char *status_json_string(int no_cache, int periodical);
extern int status_dump_file(void);
extern int status_dump_liveupgrade(void);
extern int status_read_liveupgrade(void);
extern void status_init(void);
extern void status_atend(void);

extern char *hex_encode(const char *buf, int len);
extern int hex_decode(char *obuf, int olen, const char *hex);

#endif
