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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#include "cellmalloc.h"
#include "hmalloc.h"

/*
 *   cellmalloc() -- manages arrays of cells of data 
 *
 */


struct cellhead {
	struct cellhead *next;
};


struct cellarena_t {
	int	cellsize;
	int	alignment;
	int	increment; /* alignment overhead applied.. */
	int	lifo_policy;
  	int	minfree;
	int	use_mutex;

	pthread_mutex_t mutex;

  	struct cellhead *free_head;
  	struct cellhead *free_tail;

	int	 freecount;
	int	 createsize;

	int	 cellblocks_count;
#define CELLBLOCKS_MAX 1000
	char	*cellblocks[CELLBLOCKS_MAX];	/* ref as 'char pointer' for pointer arithmetics... */
};


/*
 * new_cellblock() -- must be called MUTEX PROTECTED
 *
 */

int new_cellblock(cellarena_t *ca)
{
	int i;
	char *cb = mmap( NULL, ca->createsize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (cb == NULL || cb == (char*)-1)
	  return -1;

	if (ca->cellblocks_count >= CELLBLOCKS_MAX) return -1;

	ca->cellblocks[ca->cellblocks_count++] = cb;

	for (i = 0; i < ca->createsize-ca->increment; i += ca->increment) {
		struct cellhead *ch = (struct cellhead *)(cb + i); /* pointer arithmentic! */
		if (!ca->free_head) {
		  ca->free_head = ch;
		} else {
		  ca->free_tail->next = ch;
		}
		ca->free_tail = ch;
		ch->next = NULL;

		ca->freecount += 1;
	}

	return 0;
}



/*
 * cellinit()  -- the main program calls this once for each used cell type/size
 *
 */


cellarena_t *cellinit(int cellsize, int alignment, int policy, int createkb, int minfree)
{
	cellarena_t *ca = hmalloc(sizeof(*ca));
	memset(ca, 0, sizeof(*ca));

	ca->cellsize  = cellsize;
	ca->alignment = alignment;
	ca->minfree   = minfree;
	ca->increment = cellsize;
	if ((cellsize % alignment) != 0) {
		ca->increment +=  alignment - cellsize % alignment;
	}
	ca->lifo_policy =  policy & CELLMALLOC_POLICY_LIFO;
	ca->use_mutex   = (policy & CELLMALLOC_POLICY_NOMUTEX) ? 0 : 1;

	ca->createsize = createkb * 1024;


	pthread_mutex_init(&ca->mutex, NULL);

	new_cellblock(ca); /* First block of cells, not yet need to be mutex protected */
	while (ca->freecount < ca->minfree)
		new_cellblock(ca); /* more until minfree is full */

	return ca;
}




void *cellmalloc(cellarena_t *ca)
{
	void *cp;
	struct cellhead *ch;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	if (!ca->free_head  || (ca->freecount < ca->minfree))
		if (new_cellblock(ca)) {
			pthread_mutex_unlock(&ca->mutex);
			return NULL;
		}

	/* Pick new one off the free-head ! */
	ch = ca->free_head;
	ca->free_head = ch->next;
	cp = ch;

	ca->freecount -= 1;

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);

	return cp;
}

/*
 *  cellmallocmany() -- give many cells in single lock region
 *
 */

int   cellmallocmany(cellarena_t *ca, void **array, int numcells)
{
	int count;
	struct cellhead *ch;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	for (count = 0; count < numcells; ++count) {

		if (!ca->free_head ||
		    ca->freecount < ca->minfree) {
			/* Out of free cells ? alloc new set */
			if (new_cellblock(ca)) {
			  /* Failed ! */
			  break;
			}
		}

		/* Pick new one off the free-head ! */
		ch = ca->free_head;
		if (ch)
			ca->free_head = ch->next;

		array[count] = ch;

		ca->freecount -= 1;

	}

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);

	return count;
}



void  cellfree(cellarena_t *ca, void *p)
{
	struct cellhead *ch = (struct cellhead *)p;
	ch->next = NULL;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	if (ca->lifo_policy) {
	  /* Put the cell on free-head */
	  ch->next = ca->free_head;
	  ca->free_head = ch;

	} else {
	  /* Put the cell on free-tail */
	  if (ca->free_tail)
	    ca->free_tail->next = ch;
	  ca->free_tail = ch;
	}

	ca->freecount += 1;

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);
}

/*
 *  cellfreemany() -- release many cells in single lock region
 *
 */

void  cellfreemany(cellarena_t *ca, void **array, int numcells)
{
	int count;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	for (count = 0; count < numcells; ++count) {

	  struct cellhead *ch = (struct cellhead *)array[count];

	  if (ca->lifo_policy) {
	    /* Put the cell on free-head */
	    ch->next = ca->free_head;
	    ca->free_head = ch;

	  } else {
	    /* Put the cell on free-tail */
	    if (ca->free_tail)
	      ca->free_tail->next = ch;
	    ca->free_tail = ch;
	    ch->next = NULL;
	  }

	  ca->freecount += 1;

	}

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);
}
