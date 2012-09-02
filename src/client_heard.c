/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

 /*
  *	The clients heard list contains a list of stations heard by
  *	a given station. It's used for message routing by the
  *	message destination callsign.
  *
  *	The module also maintains a list of callsigns which have transmitted
  *	messages to a given client. The list is used to pass courtesy
  *	positions to the client.
  *	
  *	The heard list is only touched by the worker thread operating
  *	on that client socket, so it doesn't need any locking at all.
  *
  *	Igates typically have up to ~300 "heard" stations at any given
  *	moment, so we use a hash table with 16 buckets, about 18 calls
  *	in each bucket. Store and check the hash value before doing
  *	string comparisons.
  */

#include <strings.h>
#include <string.h>

#include "client_heard.h"
#include "hlog.h"
#include "config.h"
#include "hmalloc.h"
#include "cellmalloc.h"
#include "keyhash.h"

//#define HEARD_DEBUG

#ifdef HEARD_DEBUG
#define DLOG(level, fmt, ...) \
            do { hlog(level, fmt, __VA_ARGS__); } while (0)
#else
#define DLOG(fmt, ...)
#endif

#ifndef _FOR_VALGRIND_
cellarena_t *client_heard_cells;
#endif


/*
 *	Update the heard list, either update timestamp of a heard
 *	callsign or insert a new entry
 */

static void heard_list_update(struct client_t *c, struct pbuf_t *pb, struct client_heard_t **list, int *entrycount, char *which)
{
	struct client_heard_t *h;
	int call_len;
	uint32_t hash, idx;
	int i;
	
	call_len = pb->srccall_end - pb->data;
	hash = keyhashuc(pb->data, call_len, 0);
	idx = hash;
	idx ^= (idx >> 13); /* fold the hash bits.. */
	idx ^= (idx >> 26); /* fold the hash bits.. */
	i = idx % CLIENT_HEARD_BUCKETS;
	
	//DLOG(LOG_DEBUG, "heard_list_update fd %d %s: updating heard table for %.*s (hash %u i %d)", c->fd, which, call_len, pb->data, hash, i);
	
	for (h = list[i]; (h); h = h->next) {
		if (h->hash == hash && call_len == h->call_len
		    && strncasecmp(pb->data, h->callsign, h->call_len) == 0) {
			// OK, found it from the list

			//DLOG(LOG_DEBUG, "heard_list_update fd %d %s: found, updating %.*s", c->fd, which, call_len, pb->data);
			h->last_heard = pb->t;
			
			/* Because of digipeating we'll see the same station
			 * really quickly again, and the less active stations are, well, less active,
			 * let's move it in the beginning of the list to find it quicker.
			 */
			 
			 if (list[i] != h) {
				//DLOG(LOG_DEBUG, "heard_list_update fd %d %s: moving to front %.*s", c->fd, which, call_len, pb->data);
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
				
				h->next = list[i];
				h->prevp = &list[i];
				if (h->next)
					h->next->prevp = &h->next;
				list[i] = h;
			}
			
			return;
		}
	}
	
	/* Not found, insert. */
	DLOG(LOG_DEBUG, "heard_list_update fd %d %s: inserting %.*s", c->fd, which, call_len, pb->data);
#ifndef _FOR_VALGRIND_
	h = cellmalloc(client_heard_cells);
	if (!h) {
	        hlog(LOG_ERR, "heard_list_update: cellmalloc failed");
	        return;
	}
#else	
	h = hmalloc(sizeof(*h));
#endif
	h->hash = hash;
	strncpy(h->callsign, pb->data, call_len);
	h->callsign[sizeof(h->callsign)-1] = 0;
	h->call_len = call_len;
	h->last_heard = pb->t;
	
	/* insert in beginning of linked list */
	h->next = list[i];
	h->prevp = &list[i];
	if (h->next)
		h->next->prevp = &h->next;
	list[i] = h;
	*entrycount = *entrycount + 1;
}

void client_heard_update(struct client_t *c, struct pbuf_t *pb)
{
	heard_list_update(c, pb, c->client_heard, &c->client_heard_count, "heard");
}

void client_courtesy_update(struct client_t *c, struct pbuf_t *pb)
{
	heard_list_update(c, pb, c->client_courtesy, &c->client_courtesy_count, "courtesy");
}

static int heard_find(struct client_t *c, struct client_heard_t **list, int *entrycount, const char *callsign, int call_len, int storetime, int drop_if_found, char *which)
{
	struct client_heard_t *h, *next;
	uint32_t hash, idx;
	int i;
	
	hash = keyhashuc(callsign, call_len, 0);
	idx = hash;
	idx ^= (idx >> 13); /* fold the hash bits.. */
	idx ^= (idx >> 26); /* fold the hash bits.. */
	i = idx % CLIENT_HEARD_BUCKETS;
	
	//DLOG(LOG_DEBUG, "heard_find fd %d %s: checking for %.*s (hash %u i %d)", c->fd, which, call_len, callsign, hash, i);
	
	int expire_below = tick - storetime;
	next = NULL;
	
	for (h = list[i]; (h); h = next) {
		next = h->next; // we might free this one
		
		// expire old entries
		// TODO: move expiration to heard_list_update, too, it's run much more often.
		if (h->last_heard < expire_below || h->last_heard > tick) {
			DLOG(LOG_DEBUG, "heard_find fd %d %s: expiring %.*s (%ul below %ul)", c->fd, which, h->call_len, h->callsign, h->last_heard, expire_below);
			
			*h->prevp = h->next;
			if (h->next)
				h->next->prevp = h->prevp;
#ifndef _FOR_VALGRIND_
			cellfree(client_heard_cells, h);
#else
			hfree(h);
#endif
			*entrycount = *entrycount -1;
			continue;
		}
		
		if (h->hash == hash && call_len == h->call_len
		    && strncasecmp(callsign, h->callsign, h->call_len) == 0) {
			/* OK, found it from the list. */
			//DLOG(LOG_DEBUG, "heard_find fd %d %s: found %.*s%s", c->fd, which, call_len, callsign, (drop_if_found) ? " (dropping)" : "");
			
			if (drop_if_found) {
				DLOG(LOG_DEBUG, "heard_find fd %d %s: dropping %.*s%s", c->fd, which, call_len, callsign, (drop_if_found) ? " (dropping)" : "");
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
				
#ifndef _FOR_VALGRIND_
				cellfree(client_heard_cells, h);
#else
				hfree(h);
#endif
				*entrycount = *entrycount -1;
			}
			
			return 1;
		}
	}
	
	return 0;
}

int client_heard_check(struct client_t *c, const char *callsign, int call_len)
{
	return heard_find(c, c->client_heard, &c->client_heard_count,
		callsign, call_len,
		heard_list_storetime, 0, "heard");
}

int client_courtesy_needed(struct client_t *c, const char *callsign, int call_len)
{
	return heard_find(c, c->client_courtesy, &c->client_courtesy_count,
		callsign, call_len,
		courtesy_list_storetime, 1, "courtesy");
}

/*
 *	Free the whole client heard list
 */

static void heard_free_single(struct client_heard_t **list)
{
	int i;
	struct client_heard_t *n, *h;
	
	for (i = 0; i < CLIENT_HEARD_BUCKETS; i++) {
		h = list[i];
		while (h) {
			n = h->next;
#ifndef _FOR_VALGRIND_
			cellfree(client_heard_cells, h);
#else
			hfree(h);
#endif
			h = n;
		}
		list[i] = NULL;
	}
	
}

void client_heard_free(struct client_t *c)
{
	heard_free_single(c->client_heard);
	heard_free_single(c->client_courtesy);
	
	c->client_heard_count = 0;
	c->client_courtesy_count = 0;
}

void client_heard_init(void)
{
#ifndef _FOR_VALGRIND_
	client_heard_cells  = cellinit( "client_heard",
				  sizeof(struct client_heard_t),
				  __alignof__(struct client_heard_t), CELLMALLOC_POLICY_FIFO,
				  512 /* 512 KB at the time */, 0 /* minfree */ );
	/* 512 KB arena size -> about 18k entries per single arena */
#endif
}

/*
 *	cellmalloc status
 */
#ifndef _FOR_VALGRIND_
void client_heard_cell_stats(struct cellstatus_t *cellst)
{
	// TODO: this is not quite thread safe, but may be OK
	cellstatus(client_heard_cells, cellst);
}
#endif


