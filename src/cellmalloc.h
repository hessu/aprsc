/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

#ifndef _FOR_VALGRIND_ /* This does not mix with valgrind... */
#ifndef _CELLMALLOC_H_
#define _CELLMALLOC_H_

/*
 *   cellmalloc() -- manages arrays of cells of data 
 *
 */

typedef struct cellarena_t cellarena_t;

extern cellarena_t *cellinit(int cellsize, int alignment, int policy, int createkb, int minfree);

#define CELLMALLOC_POLICY_FIFO    0
#define CELLMALLOC_POLICY_LIFO    1
#define CELLMALLOC_POLICY_NOMUTEX 2

extern void *cellmalloc(cellarena_t *cellarena);
extern int   cellmallocmany(cellarena_t *cellarena, void **array, int numcells);
extern void  cellfree(cellarena_t *cellarena, void *p);
extern void  cellfreemany(cellarena_t *cellarena, void **array, int numcells);


#endif
#else /* _FOR_VALGRIND_  .. normal malloc/free is better */

#include "hmalloc.h"

#endif
