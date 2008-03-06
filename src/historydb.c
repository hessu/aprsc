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
#include <math.h>

#include "hlog.h"
#include "worker.h"
#include "cellmalloc.h"
#include "historydb.h"
#include "hmalloc.h"
#include "crc32.h"

cellarena_t *historydb_cells;


// FIXME: Possibly multiple parallel locks (like 1000 ?) that keep
// control on a subset of historydb hash bucket chains ???
// Note: mutex lock size is about 1/4 of rwlock size...

rwlock_t historydb_rwlock;

struct history_cell_t **historydb_hash;
int historydb_hash_modulo;
int historydb_maxage = 48*3600; // 48 hours

void historydb_init(void)
{
	int i;

	rwl_init(&historydb_rwlock);

	// printf("historydb_init() sizeof(mutex)=%d sizeof(rwlock)=%d\n",
	//       sizeof(pthread_mutex_t), sizeof(rwlock_t));

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


void historydb_dump_entry(FILE *fp, struct history_cell_t *hp)
{
	fprintf(fp, "%ld\t", hp->arrivaltime);
	fprintf(fp, "%s\t", hp->key);
	fprintf(fp, "%d\t%d\t", hp->packettype, hp->flags);
	fprintf(fp, "%f\t%f\t", hp->lat, hp->lon);
	fprintf(fp, "%d\t", hp->packetlen);
	fwrite(hp->packet, hp->packetlen, 1, fp); // with terminating CRLF
}

void historydb_dump(FILE *fp)
{
	// Dump the historydb out on text format for possible latter reload
	int i;
	struct history_cell_t *hp;
	time_t expirytime   = now - historydb_maxage;

	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_rdlock(&historydb_rwlock);

	for ( i = 0; i < historydb_hash_modulo; ++i ) {
		hp = historydb_hash[i];
		for ( ; hp ; hp = hp->next )
			if (hp->arrivaltime > expirytime)
				historydb_dump_entry(fp, hp);
	}

	// Free the lock
	rwl_rdunlock(&historydb_rwlock);
}

int historydb_load(FILE *fp)
{
	// load the historydb in text format, ignore too old positions
	int i;
	time_t expirytime   = now - historydb_maxage;
	char bufline[2000]; // should be enough...
	time_t t;
	char keybuf[20];
	int packettype, flags;
	float lat, lon;
	int packetlen = 0;
	int h1;
	struct history_cell_t **hpp;
	struct history_cell_t *hp, *hp1;
	int linecount = 0;


	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_wrlock(&historydb_rwlock);

	// discard previous content
	for ( i = 0; i < historydb_hash_modulo; ++i ) {
		struct history_cell_t *hp, *nhp;

		hp = historydb_hash[i];
		nhp = NULL;
		for ( ; hp ; hp = nhp ) {
			// the 'next' will get freed from underneath of us..
			nhp = hp->next;
			historycell_free(hp);
		}
		historydb_hash[i] = NULL;
	}

	// now the loading...
	while (!feof(fp)) {

		*bufline = 0;

		i = fscanf(fp, "%ld\t%9[^\t]\t%d\t%d\t%f\t%f\t%d\t",
			   &t,    keybuf, &packettype, &flags,
			   &lat, &lon,    &packetlen );

		if (i != 7) {  // verify correct scan
			hlog(LOG_ERR, "historybuf load, wrong parse on line %d", linecount);
			break;
		}
		// FIXME: now several parameters may be invalid for us, like:
		//   if packetlen >= sizeof(bufline)  ??

		i = fread( bufline, packetlen, 1, fp);

		// i == packetlen ??

		if (t <= expirytime)
			continue;	// Too old, forget it.
		
		h1 = historydb_hash1(keybuf);
		i = h1 % historydb_hash_modulo;

		hp1 = NULL;
		hpp = &historydb_hash[i];

		// scan the hash-bucket chain
		while (( hp = *hpp )) {
			if ( (hp->hash1 == h1) &&
			       // Hash match, compare the key
			       (strcmp(hp->key, keybuf) == 0) ) {
			  	// Key match! -- should not happen while loading..
				// Update the data content
				hp1 = hp;
				hp->lat         = lat;
				hp->coslat      = cosf(lat);
				hp->lon         = lon;
				hp->arrivaltime = t;
				hp->packettype  = packettype;
				hp->flags       = flags;
				hp->packetlen   = packetlen;
				memcpy(hp->packet, bufline,
				       packetlen > 300 ? 300 : packetlen);
				break;
			}
			// advance hpp..
			hpp = &(hp -> next);
		}
		if (!hp1) {
			// Not found on this chain, insert it!
			hp = historycell_alloc(packetlen);
			hp->next = NULL;
			hp->hash1 = h1;
			strcpy(hp->key, keybuf);
			hp->lat         = lat;
			hp->coslat      = cosf(lat);
			hp->lon         = lon;
			hp->arrivaltime = t;
			hp->packettype  = packettype;
			hp->flags       = flags;
			hp->packetlen   = packetlen;
			memcpy(hp->packet, bufline, packetlen);

			*hpp = hp;
		}
	} // .. while !feof ..

	// Free the lock
	rwl_wrunlock(&historydb_rwlock);

	return 0;
}


/* insert... */

int historydb_insert(struct pbuf_t *pb)
{
	int i;
	uint32_t h1;
	int isdead = 0;
	struct history_cell_t **hp, *cp, *cp1;

	time_t expirytime   = now - historydb_maxage;

	char keybuf[CALLSIGNLEN_MAX+1];
	char *s;

	if (!(pb->flags & F_HASPOS))
		return -1; // No positional data...

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

	cp = cp1 = NULL;
	hp = &historydb_hash[i];

	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_wrlock(&historydb_rwlock);

	// scan the hash-bucket chain, and do incidential obsolete data discard
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
				memcpy(cp->packet, pb->data,
				       cp->packetlen > 300 ? 300 : cp->packetlen);
				// Continue scanning whole chain for possible obsolete items
			}
		} // .. else no match, advance hp..
		hp = &(cp -> next);
	}

	if (!cp1 && !isdead) {
		// Not found on this chain, append it!
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
		*hp = cp; 
	}

	// Free the lock
	rwl_wrunlock(&historydb_rwlock);

	return 1;
}

/* lookup... */

int historydb_lookup(const char *keybuf, struct history_cell_t **result)
{
	int i;
	uint32_t h1;
	struct history_cell_t **hp, *cp, *cp1;

	// validity is 5 minutes shorter than expiration time..
	time_t validitytime   = now - historydb_maxage - 5*60;

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
			     (cp->arrivaltime > validitytime)
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
	*result = cp1;

	if (!cp1) return 0;  // Not found anything

	return 1;
}

