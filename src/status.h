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

extern time_t startup_tick;

extern char *status_json_string(int no_cache, int periodical);
extern int status_dump_file(void);
extern int status_dump_liveupgrade(void);
extern void status_init(void);
extern void status_atend(void);

#endif
