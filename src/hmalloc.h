/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef HMALLOC_H
#define HMALLOC_H

#include <stdio.h>
#include <stdlib.h>

/*
 *	Replacements for malloc, realloc and free, which never fail,
 *	and might keep statistics on memory allocation...
 */

extern void *hmalloc(size_t size);
extern void *hrealloc(void *ptr, size_t size);
extern void hfree(void *ptr);

extern char *hstrdup(const char *s);

#endif

