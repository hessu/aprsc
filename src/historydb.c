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
#include "crc32.h"



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
 *
 *	In APRS-IS there are about 25 000 distinct callsigns or
 *	item or object names with position information PER WEEK.
 *	DB lifetime of 48 hours cuts that down a bit more.
 *	With 300 byte packet buffer in the entry (most of which is
 *	never used), the history-db size is around 8-9 MB in memory.
 */

cellarena_t *historydb_cells;

      struct history_cell_t {
	struct history_cell_t *next;

	time_t   arrivaltime;
	char     key[CALLSIGNLEN_MAX+1];
	uint32_t hash1, hash2;

	float	lat, coslat, lon;

	int  packettype;
	int  flags;

	int  packetlen;
	char packet[300];
	/* FIXME: is this enough, or should there be two different
	   sizes of cells ?  See  pbuf_t cells... */
};


// FIXME: Possibly multiple parallel locks (like 1000 ?) that keep
// control on a subset of historydb hash bucket chains ???

rwlock_t historydb_rwlock;

struct history_cell_t **historydb_hash;
int historydb_hash_modulo;
int historydb_maxage = 48*3600; // 48 hours

void historydb_init(void)
{
	int i;

	rwl_init(&historydb_rwlock);

	historydb_cells = cellinit( sizeof(struct history_cell_t),
				    __alignof__(struct history_cell_t), 
				    0 /* FIFO! */, 2048 /* 2 MB */,
				    0 );

	historydb_hash_modulo = 8192 ; // FIXME: is this acceptable or not ?

	i = sizeof(struct history_cell_t *) * historydb_hash_modulo;
	historydb_hash = hmalloc(i);
	memset(historydb_hash, 0, i);
}

// Called only under WR-LOCK
void historycell_free(struct history_cell_t *p)
{
	cellfree( historydb_cells, p );
}

// Called only under WR-LOCK
struct history_cell_t *historycell_alloc(int packet_len)
{
	struct history_cell_t *hp;

	hp = cellmalloc( historydb_cells );

	return hp;
}


uint32_t historydb_hash1(const char *s) 
{
	// FIXME: hash function !
	return crc32((const void *)s);
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
	int i;
	uint32_t h1;
	int isdead = 0;
	struct history_cell_t **hp, *cp, *cp1;
	time_t expirytime = now - historydb_maxage;

	char keybuf[CALLSIGNLEN_MAX+1];
	char *s;


	// FIXME: if (pb->packet_len > 300)  ???


	keybuf[CALLSIGNLEN_MAX] = 0;
	if (pb->packettype & T_OBJECT) {
	  // Pick object name  ";item  *"
	  memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
	  s = strchr(keybuf, '*');
	  if (s) *s = 0;
	  else {
	    s = strchr(keybuf, '_');
	    if (s) {
	      *s = 0;
	      isdead = 1;
	    }
	  }
	  s = keybuf + strlen(keybuf);
	  for ( ; s > keybuf; --s ) {  // tail space padded..
	    if (*s == ' ') *s = ' ';
	    else break;
	  }

	} else if (pb->packettype & T_ITEM) {
	  // Pick item name  ") . . . !"  or ") . . . _"
	  memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
	  s = strchr(keybuf, '!');
	  if (s) *s = 0;
	  else {
	    s = strchr(keybuf, '_');
	    if (s) {
	      *s = 0;
	      isdead = 1;
	    }
	  }
	} else if (pb->packettype & T_POSITION) {
	  // Pick originator callsign
	  memcpy( keybuf, pb->data, CALLSIGNLEN_MAX) ;
	  s = strchr(keybuf, '>');
	  if (s) *s = 0;
	} else {
	  return -1; // Not a packet with positional data, not interested in...
	}

	h1 = historydb_hash1(keybuf);
	i = h1 % historydb_hash_modulo;

	cp1 = NULL;
	hp = &historydb_hash[i];

	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_wrlock(&historydb_rwlock);

	if (*hp) {
	  while (( cp = *hp )) {
	    if (cp->arrivaltime < expirytime) {
	      // OLD...
	      *hp = cp->next;
	      cp->next = NULL;
	      historycell_free(cp);
	      continue;
	    }
	    if ( (cp->hash1 == h1) &&
		 // Hash match, compare the key
		 (strcmp(cp->key, keybuf) == 0) ) {
	      // Key match!
	      if (isdead) {
		// Remove this key..
		*hp = cp->next;
		cp->next = NULL;
		historycell_free(cp);
		continue;

	      } else {
		// Update the data content
		cp1 = cp;
		cp->lat         = pb->lat;
		cp->coslat      = pb->cos_lat;
		cp->lon         = pb->lng;
		cp->arrivaltime = pb->t;
		cp->packettype  = pb->packettype;
		cp->flags       = pb->flags;
		cp->packetlen   = pb->packet_len;
		memcpy(cp->packet, pb->data, cp->packetlen > 300 ? 300 : cp->packetlen);
		// Continue scanning whole chain for possible obsolete items
	      }
	    }

	    hp = &(cp -> next);
	  }
	  if (!cp && !cp1) {
	    // Not found on this chain, insert it!
	    cp = historycell_alloc(pb->packet_len);
	    cp->next = NULL;
	    cp->hash1 = h1;
	    strcpy(cp->key, keybuf);

	    cp->lat         = pb->lat;
	    cp->coslat      = pb->cos_lat;
	    cp->lon         = pb->lng;
	    cp->arrivaltime = pb->t;
	    cp->packettype  = pb->packettype;
	    cp->flags       = pb->flags;
	    cp->packetlen   = pb->packet_len;
	    memcpy(cp->packet, pb->data, cp->packetlen);
	  }
	} else if (!isdead) {
	  // Empty hash chain root, insert first item..
	  cp = historycell_alloc(pb->packet_len);
	  cp->next = NULL;
	  cp->hash1 = h1;
	  strcpy(cp->key, keybuf);

	  cp->lat         = pb->lat;
	  cp->coslat      = pb->cos_lat;
	  cp->lon         = pb->lng;
	  cp->arrivaltime = pb->t;
	  cp->packettype  = pb->packettype;
	  cp->flags       = pb->flags;
	  cp->packetlen   = pb->packet_len;
	  memcpy(cp->packet, pb->data, cp->packetlen);
	}


	// Free the lock
	rwl_wrunlock(&historydb_rwlock);

	return 1;
}

/* lookup... interface yet in state of flux.. */

int historydb_lookup(const char *keybuf, void *p)
{
	int i;
	uint32_t h1;
	struct history_cell_t **hp, *cp, *cp1;
	time_t expirytime = now - historydb_maxage;

	h1 = historydb_hash1(keybuf);
	i = h1 % historydb_hash_modulo;

	cp1 = NULL;
	hp = &historydb_hash[i];

	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_rdlock(&historydb_rwlock);

	if (*hp) {
	  while (( cp = *hp )) {
	    if ( (cp->hash1 == h1) &&
		 // Hash match, compare the key
		 (strcmp(cp->key, keybuf) == 0)  &&
		 // Key match!
		 (cp->arrivaltime > expirytime)
		 // NOT too old..
		 ) {
	      cp1 = cp;
	      break;
	    }
	    // Pick next possible item in hash chain
	    hp = &(cp -> next);
	  }
	}


	// Free the lock
	rwl_rdunlock(&historydb_rwlock);

	// cp1 variable has the result
	if (!cp1) return -1;  // Not found anything

	// FIXME: return the data somehow...

	return 1;
}

