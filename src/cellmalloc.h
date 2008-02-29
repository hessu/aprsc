#ifndef _SLABMALLOC_H_
#define _SLABMALLOC_H_

/*
 *   cellmalloc() -- manages arrays of cells of data 
 *
 */

typedef struct cellarena_t cellarena_t;

extern cellarena_t *cellinit(int cellsize, int alignment, int lifo_policy, int createkb);

extern void *cellmalloc(cellarena_t *cellarena);
extern int   cellmallocmany(cellarena_t *cellarena, void **array, int numcells);
extern void  cellfree(cellarena_t *cellarena, void *p);
extern void  cellfreemany(cellarena_t *cellarena, void **array, int numcells);


#endif
