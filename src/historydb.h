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


struct history_cell_t {
	struct history_cell_t *next;

	time_t   arrivaltime;
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


extern void historydb_init(void);

extern void historydb_dump(FILE *fp);

extern void historydb_cleanup(void);
extern void historydb_atend(void);

/* insert and lookup... interface yet unspecified */
extern int historydb_insert(struct pbuf_t*);
extern int historydb_lookup(const char *keybuf, const int keylen, struct history_cell_t **result);

#endif
