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

#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "hlog.h"
#include "worker.h"
#include "cellmalloc.h"
#include "historydb.h"
#include "hmalloc.h"



/*
 *	The historydb contains positional packet data in form of:
 *	  - position packet
 *	  - objects
 *	  - items
 *	Keying varies, origination callsign of positions, name
 *	for object/item.
 *
 *	Uses RW-locking, W for inserts/cleanups, R for lookups.
 *
 *	Inserting does incidential cleanup scanning while traversing
 *	hash chains.
 */

cellarena_t *historydb_cells;

      struct history_cell_t {
	struct history_cell_t *next;

	time_t   arrivaltime;
	char     key[CALLSIGNLEN_MAX+1];
	uint32_t hash, hash2;

	float	lat, coslat, lon;

	int  packettype;
	int  flags;

	int  length;
	char packet[200];
	/* FIXME: is this enough, or should there be two different
	   sizes of cells ?  See  pbuf_t cells... */
};


rwlock_t historydb_rwlock = RWL_INITIALIZER;

struct history_cell_t *historydb_hash;
int historydb_hash_modulo;

void historydb_init(void)
{
	int i;

	historydb_cells = cellinit( sizeof(struct history_cell_t),
				    __alignof__(struct history_cell_t), 
				    0 /* FIFO! */, 2048 /* 2 MB */,
				    0 );

	historydb_hash_modulo = 8192 ; // FIXME: is this acceptable or not ?

	i = sizeof(struct history_cell_t *) * historydb_hash_modulo;
	historydb_hash = hmalloc(i);
	memset(historydb_hash, 0, i);
}

int historydb_dump(FILE *fp)
{
  // Dump the historydb out on text format for possible latter reload
  return -1;
}

int historydb_load(FILE *fp)
{
  // load the historydb in text format, ignore too old positions
  return -1;
}


/* insert... interface yet in state of flux.. */

int historydb_insert(struct pbuf_t *pb)
{
  return -1;
}

/* lookup... interface yet in state of flux.. */

int historydb_lookup(void*p)
{
  return -1;
}

