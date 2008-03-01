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
/*
** sptree.h:  The following type declarations provide the binary tree
**  representation of event-sets or priority queues needed by splay trees
**
**  assumes that data and datb will be provided by the application
**  to hold all application specific information
**
**  assumes that key will be provided by the application, comparable
**  with the compare function applied to the addresses of two keys.
*/

# ifndef SPTREE_H
# define SPTREE_H

#ifndef __
# ifdef __STDC__
#  define __(x) x
# else
#  define __(x) ()
# endif
#endif

#include <inttypes.h>
#include "worker.h"

typedef uint32_t	spkey_t;

struct spblk {
	struct spblk	*leftlink;
	struct spblk	*rightlink;
	struct spblk	*uplink;
	void		*data;
	spkey_t		key;
};

struct sptree {
	struct spblk	*root;		/* root node */
	struct sptree	*symbols;	/* If this db needs symbol support,
					   here is another sptree for those */
	long long	eltscnt;	/* How many elements in this tree */
	long long	lookups;	/* number of splookup()s */
	long long	lkpcmps;	/* number of lookup comparisons */
	long long	enqs;		/* number of spenq()s */
	long long	enqcmps;	/* compares in spenq */
	long long	splays;
	long long	splayloops;
};

extern long sp_allocated;
extern long sp_entries;

extern void sp_free_freelist(void);
extern struct sptree *sp_init __((void)); /* init tree */
extern struct spblk *sp_lookup __((spkey_t key,
				   struct sptree *q));	/* find key in a tree*/
extern struct spblk *sp_install __((spkey_t key, struct sptree *q)); /* enter an item,
							   allocating or replacing */
extern void sp_scan __((int (*f)(struct spblk *), struct spblk *n,
			struct sptree *q));	/* scan forward through tree */
extern void sp_delete __((struct spblk *n, struct sptree *q)); /* delete node from tree */
extern void        sp_null __((struct sptree *));
extern const char *sp_stats __((struct sptree *q));/* return tree statistics */
extern spkey_t     symbol __((const void *s));	/* build this into a symbol */
extern spkey_t     symbol_lookup __((const void *s));
extern spkey_t     symbol_db        __((const void *, struct sptree *));
extern spkey_t     symbol_lookup_db __((const void *, struct sptree *));
extern spkey_t     symbol_db_mem    __((const void *, int, struct sptree *));
extern spkey_t symbol_lookup_db_mem __((const void *, int, struct sptree *));
extern void	   symbol_free_db __((const void *, struct sptree *));
extern void	   symbol_null_db __((struct sptree *));

extern struct spblk *lookup_incoresp __((const char *, struct sptree *));
extern int      add_incoresp __((const char *, const char *, struct sptree *));
extern int     addd_incoresp __((const char *, const void *, struct sptree *));

extern const char *pname  __((spkey_t id));
#ifdef MALLOC_TRACE
extern int  icpname    __((struct spblk *spl));
extern void prsymtable __((void))
#endif

extern struct spblk * sp_fhead __((struct sptree *));
			/* fast non-splaying head */
extern struct spblk * sp_fnext __((struct spblk *));
			/* fast non-splaying next */

#endif	/* SPTREE_H */
