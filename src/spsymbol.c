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
 *	Copyright 1988 by Rayan S. Zachariassen, all rights reserved.
 *	This will be free software, but only when it is finished.
 */

/*
 * Symbol routine: it maps an arbitrary string into a unique key
 * that can be used as an index into various tables.
 */

#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "splay.h"
#include "hmalloc.h"
#include "crc32.h"

#ifdef	symbol
#undef	symbol
#endif	/* symbol */

struct syment {
	struct syment *next;
	int        namelen;
	const char name[1];
};

struct sptree *spt_symtab = NULL;

spkey_t symbol(const void *s)
{
	if (spt_symtab == NULL)
		spt_symtab = sp_init();
		
	return symbol_db(s, spt_symtab);
}

spkey_t symbol_lookup_db_mem(const void *s, const int slen, struct sptree *spt)
{
	register spkey_t key;
	register struct syment *se, *pe;
	struct spblk *spl;

	if (s == NULL)
		return 0;

	key = crc32n(s, slen, 0);

	/* Ok, time for the hard work.  Lets see if we have this key
	   in the symtab splay tree */

	pe = NULL;
	spl = sp_lookup(key, spt);
	if (spl != NULL) {
		/* Got it !  Now see that we really have it, and
		   not only have a hash collision */

		se = (struct syment *)spl->data;
		do {
			if (se->namelen == slen &&
			    memcmp(se->name, s, slen) == 0) {
				/* Really found it! */
				return (spkey_t)se;
			}
			pe = se;
			se = se->next;
		} while (se != NULL);
	}
	return 0;
}

spkey_t symbol_db_mem(const void *s, int slen, struct sptree *spt)
{
	register spkey_t key;
	register struct syment *se, *pe;
	struct spblk *spl;

	if (s == NULL)
		return 0;

	key = crc32n(s,slen, 0);

	/* Ok, time for the hard work.  Lets see if we have this key
	   in the symtab splay tree */

	pe = NULL;
	spl = sp_lookup(key, spt);
	if (spl != NULL) {
		/* Got it !  Now see that we really have it, and
		   not only have a hash collision */

		se = (struct syment *)spl->data;
		do {
			if (se->namelen == slen &&
			    memcmp(se->name, s, slen) == 0) {
				/* Really found it! */
				return (spkey_t)se;
			}
			pe = se;
			se = se->next;
		} while (se != NULL);
	}
	se = (struct syment *)hmalloc(sizeof (struct syment) + slen);
	memcpy((void*)se->name, s, slen);
	((char*)se->name)[slen] = 0;
	se->namelen = slen;
	se->next    = NULL;
	if (pe != NULL)
		pe->next = se;
	else {
		spl = sp_install(key, spt);
		spl->data = (void *)se;
	}
	return (spkey_t)se;
}

/*
 * Empty the entire symbol splay tree
 */
static int symbol_null(struct spblk *spl)
{
	struct syment *se, *sn;
	se = (struct syment *)spl->data;
	for (sn = se ? se->next : NULL; se != NULL; se = sn) {
	  sn = se->next;
	  hfree(se);
	}
	return 0;
}

void symbol_null_db(struct sptree *spt)
{
#if 0
	idname = "";
	idkey  = 0;
#endif
	sp_scan(symbol_null, (struct spblk *)NULL, spt);
	sp_null(spt);
}


/*
 * Remove named symbol from the splay tree
 */

void symbol_free_db_mem(const void *s, int slen, struct sptree *spt)
{
	register spkey_t key;
	register struct syment *se, *pe;
	struct spblk *spl;
	
	if (s == NULL || spt == NULL)
		return;
	
	key = crc32n(s, slen, 0);
	
	/* Ok, time for the hard work.  Lets see if we have this key
	   in the symtab splay tree (we can't use cache here!) */
	
	pe = NULL;
	spl = sp_lookup(key, spt);
	if (spl != NULL) {
		/* Got it !  Now see that we really have it, and
		   not only have a hash collision */

		se = (struct syment *)spl->data;
		do {
		  if (se->namelen == slen &&
		      memcmp(se->name, s, slen) == 0) {
		    /* Really found it! */
		    if (pe != NULL)
		      pe->next = se->next;
		    else
		      spl->data = (void*) se->next;
		    hfree(se);
		    break;
		  }
		  pe = se;
		  se = se->next;
		} while (se != NULL);
	}

	if (spl != NULL && spl->data == NULL)
		sp_delete(spl, spt);
}

void symbol_free_db(const void *s, struct sptree *spt)
{
	if (s == NULL || spt == NULL)
		return;
		
	symbol_free_db_mem(s, strlen(s), spt);
}


/*
 * Return a printable string representation of the symbol whose key is passed.
 */

const char *pname(spkey_t id)
{
	return (const char *)((struct syment *)id)->name;
}

spkey_t symbol_lookup_db(const void *s, struct sptree *spt)
{
	if (s == NULL)
		return 0;

	return symbol_lookup_db_mem(s, strlen(s), spt);
}

spkey_t symbol_lookup(const void *s)
{
	if (spt_symtab == NULL)
		spt_symtab = sp_init();
		
	return symbol_lookup_db(s, spt_symtab);
}

spkey_t symbol_db(const void *s, struct sptree *spt)
{
	if (s == NULL)
		return 0;
		
	return symbol_db_mem(s, strlen(s), spt);
}

