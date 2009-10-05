/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

/*
 *	Replacements for malloc, realloc and free, which never fail,
 *	and might keep statistics on memory allocation...
 */

#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "hmalloc.h"

int mem_panic = 0;

void *hmalloc(size_t size)
{
	void *p;
	
	if (!(p = malloc(size))) {
		if (mem_panic)
			exit(1);	/* To prevent a deadlock */
		mem_panic = 1;
		fprintf(stderr, "hmalloc: Out of memory! Could not allocate %d bytes.", (int)size);
		exit(1);
	}
	
	return p;
}

void *hrealloc(void *ptr, size_t size)
{
	void *p;
	
	if (!(p = realloc(ptr, size))) {
		if (mem_panic)
			exit(1);
		mem_panic = 1;
		fprintf(stderr, "hrealloc: Out of memory! Could not reallocate %d bytes.", (int)size);
		exit(1);
	}
	
	return p;
}

void hfree(void *ptr)
{
	if (ptr)
		free(ptr);
}

char *hstrdup(const char *s)
{
	char *p;
	
	p = hmalloc(strlen(s)+1);
	strcpy(p, s);
	
	return p;
}

