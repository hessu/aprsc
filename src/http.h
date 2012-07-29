/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef HTTP_H
#define HTTP_H

extern struct worker_t *http_worker;

extern int http_reconfiguring;
extern int http_shutting_down;

extern void http_thread(void *asdf);

#endif
