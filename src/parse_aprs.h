/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef PARSE_APRS_H
#define PARSE_APRS_H

#include "worker.h"
#include "parse_aprs.h"

extern int parse_aprs(struct worker_t *self, struct pbuf_t *pb);

#endif
