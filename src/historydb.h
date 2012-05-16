/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */



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
 *	Memory usage is around 3-4 MB
 */

#ifndef __HISTORYDB_H__
#define __HISTORYDB_H__

#include "cellmalloc.h"

struct history_cell_t {
	struct history_cell_t *next;

	time_t   arrivaltime;
	int	 keylen;
	char     key[CALLSIGNLEN_MAX+2];
	uint32_t hash1;

	float	lat, coslat, lon;

	int  packettype;
	int  flags;

	int  packetlen;
	char *packet;
	char packetbuf[170]; /* Maybe a dozen packets are bigger than
				170 bytes long out of some 17 000 .. */
};

#define HISTORYDB_CELL_SIZE sizeof(struct history_cell_t)

extern long historydb_cellgauge;

extern void historydb_init(void);

extern void historydb_dump(FILE *fp);

extern void historydb_cleanup(void);
extern void historydb_atend(void);

/* insert and lookup... interface yet unspecified */
extern int historydb_insert(struct pbuf_t*);
extern int historydb_lookup(const char *keybuf, const int keylen, struct history_cell_t **result);

/* cellmalloc status */
extern void historydb_cell_stats(struct cellstatus_t *cellst);

#endif
