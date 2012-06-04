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

extern char *status_json_string(void);
extern int status_dump_file(void);

#endif
