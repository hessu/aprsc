/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef PARSE_QC_H
#define PARSE_QC_H

#include "worker.h" /* struct client_t */

extern int check_invalid_q_callsign(const char *call, int len);

extern int q_process(struct client_t *c, const char *pdata,
                     char *new_q, int new_q_size, char *via_start,
                     char **path_end, int pathlen, char **new_q_start, char **q_replace,
                     int originated_by_client);

#endif
