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
  *	The client's heard list contains a list of stations heard by
  *	a given station. It's used for message routing by the
  *	message destination callsign.
  *
  *	The heard list is only touched by the worker thread operating
  *	on that client socket, so it shouldn't need any locking at all.
  *
  *	TODO: The client heard list is now a linked list. Should probably have a hash.
  */

#include <strings.h>
#include <string.h>

#include "client_heard.h"
#include "hlog.h"
#include "config.h"
#include "hmalloc.h"

/*
 *	Update the heard list, either update timestamp of a heard
 *	callsign or insert a new entry
 */

static void heard_list_update(struct client_t *c, struct pbuf_t *pb, struct client_heard_t **list, int *entrycount, char *which)
{
	struct client_heard_t *h;
	int call_len;
	
	call_len = pb->srccall_end - pb->data;
	
	hlog(LOG_DEBUG, "heard_list_update fd %d %s: updating heard table for %.*s", c->fd, which, call_len, pb->data);
	
	for (h = *list; (h); h = h->next) {
		if (call_len == h->call_len
		    && strncasecmp(pb->data, h->callsign, h->call_len) == 0) {
			// OK, found it from the list
			hlog(LOG_DEBUG, "heard_list_update fd %d %s: found, updating %.*s", c->fd, which, call_len, pb->data);
			h->last_heard = pb->t;
			
			/* Because of digipeating we'll see the same station
			 * really quickly again, and the less active stations are, well, less active,
			 * let's move it in the beginning of the list to find it quicker.
			 */
			 
			 if (c->client_heard != h) {
				hlog(LOG_DEBUG, "heard_list_update fd %d %s: moving to front %.*s", c->fd, which, call_len, pb->data);
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
				
				h->next = *list;
				h->prevp = list;
				if (h->next)
					h->next->prevp = &h->next;
				*list = h;
			}
			
			return;
		}
	}
	
	/* Not found, insert. */
	hlog(LOG_DEBUG, "heard_list_update fd %d %s: inserting %.*s", c->fd, which, call_len, pb->data);
	h = hmalloc(sizeof(*h));
	strncpy(h->callsign, pb->data, call_len);
	h->callsign[sizeof(h->callsign)-1] = 0;
	h->call_len = call_len;
	h->last_heard = pb->t;
	
	/* insert in beginning of linked list */
	h->next = *list;
	h->prevp = list;
	if (h->next)
		h->next->prevp = &h->next;
	*list = h;
	*entrycount = *entrycount + 1;
}

void client_heard_update(struct client_t *c, struct pbuf_t *pb)
{
	heard_list_update(c, pb, &c->client_heard, &c->client_heard_count, "heard");
}

void client_courtesy_update(struct client_t *c, struct pbuf_t *pb)
{
	heard_list_update(c, pb, &c->client_courtesy, &c->client_courtesy_count, "courtesy");
}

static int heard_find(struct client_t *c, struct client_heard_t **list, int *entrycount, const char *callsign, int call_len, int storetime, int drop_if_found, char *which)
{
	struct client_heard_t *h, *next;
	
	hlog(LOG_DEBUG, "heard_find fd %d %s: checking for %.*s", c->fd, which, call_len, callsign);
	
	int expire_below = tick - storetime;
	next = NULL;
	
	for (h = *list; (h); h = next) {
		next = h->next; // we might free this one
		
		// expire old entries
		if (h->last_heard < expire_below || h->last_heard > tick) {
			hlog(LOG_DEBUG, "heard_find fd %d %s: expiring %s (%ul below %ul)", c->fd, which, h->callsign, h->last_heard, expire_below);
			
			*h->prevp = h->next;
			if (h->next)
				h->next->prevp = h->prevp;
			
			hfree(h);
			*entrycount = *entrycount -1;
			continue;
		}
		
		if (call_len == h->call_len
		    && strncasecmp(callsign, h->callsign, h->call_len) == 0) {
			/* OK, found it from the list. */
			hlog(LOG_DEBUG, "heard_find fd %d %s: found %.*s%s", c->fd, which, call_len, callsign, (drop_if_found) ? " (dropping)" : "");
			
			if (drop_if_found) {
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
				
				hfree(h);
				*entrycount = *entrycount -1;
			}
			
			return 1;
		}
	}
	
	return 0;
}

int client_heard_check(struct client_t *c, const char *callsign, int call_len)
{
	return heard_find(c, &c->client_heard, &c->client_heard_count,
		callsign, call_len,
		heard_list_storetime, 0, "heard");
}

int client_courtesy_needed(struct client_t *c, const char *callsign, int call_len)
{
	return heard_find(c, &c->client_courtesy, &c->client_courtesy_count,
		callsign, call_len,
		courtesy_list_storetime, 1, "courtesy");
}

/*
 *	Free the whole client heard list
 */

void client_heard_free(struct client_t *c)
{
	struct client_heard_t *h;
	
	while (c->client_heard) {
		h = c->client_heard->next;
		hfree(c->client_heard);
		c->client_heard = h;
		c->client_heard_count--;
	}
	
	while (c->client_courtesy) {
		h = c->client_courtesy->next;
		hfree(c->client_courtesy);
		c->client_courtesy = h;
		c->client_courtesy_count--;
	}
}
