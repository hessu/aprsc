/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef _FOR_VALGRIND_ /* This does not mix with valgrind... */
#ifndef _CELLMALLOC_H_
#define _CELLMALLOC_H_

/*
 *   cellmalloc() -- manages arrays of cells of data 
 *
 */

struct cellstatus_t {
	int cellsize;
	int alignment;
	int cellsize_aligned;
	int cellcount;
	int freecount;
	int blocks;
	int blocks_max;
	int block_size;
};

typedef struct cellarena_t cellarena_t;

extern cellarena_t *cellinit(const char *arenaname, const int cellsize, const int alignment, const int policy, const int createkb, const int minfree);

#define CELLMALLOC_POLICY_FIFO    0
#define CELLMALLOC_POLICY_LIFO    1
#define CELLMALLOC_POLICY_NOMUTEX 2

extern void *cellmalloc(cellarena_t *cellarena);
extern int   cellmallocmany(cellarena_t *cellarena, void **array, const int numcells);
extern void  cellfree(cellarena_t *cellarena, void *p);
extern void  cellfreemany(cellarena_t *cellarena, void **array, const int numcells);
extern void  cellstatus(cellarena_t *cellarena, struct cellstatus_t *status);

#endif
#else /* _FOR_VALGRIND_  .. normal malloc/free is better */

#include "hmalloc.h"

#endif
