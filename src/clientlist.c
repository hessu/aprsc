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
  *	Clientlist contains a list of connected clients, along with their
  *	verification status. It is updated when clients connect or disconnect,
  *	and lookups are made more often by the Q construct algorithm.
  *
  *	This list is maintained so that the login handler can find duplicate
  *	verified logins, and that the Q construct handler can lookup
  *	verified logins without locking and walking the other worker thread's
  *	client list.
  *
  *	TODO: the clientlist should use a hash for quick lookups
  *	TODO: the clientlist should use cellmalloc
  */

#include <string.h>

#include "clientlist.h"
#include "hmalloc.h"
#include "rwlock.h"
#include "keyhash.h"
#include "hlog.h"

//#define CLIENTLIST_DEBUG

#ifdef CLIENTLIST_DEBUG
#define DLOG(fmt, ...) \
            do { hlog(LOG_DEBUG, fmt, __VA_ARGS__); } while (0)
#else
#define DLOG(fmt, ...)
#endif


struct clientlist_t {
	struct clientlist_t *next;
	struct clientlist_t **prevp;
	
	char  username[16];     /* The callsign */
	int   username_len;
	int   validated;	/* Valid passcode given? */
	int   fd;               /* File descriptor, can be used by another
	                           thread to shut down a socket */
	
	uint32_t hash;		/* hash value */
	void *client_id;	/* DO NOT REFERENCE - just used for an ID */
};

/* hash buckets - serious overkill, but great for 40k clients */
#define CLIENTLIST_BUCKETS 512
struct clientlist_t *clientlist[CLIENTLIST_BUCKETS];
rwlock_t clientlist_lock = RWL_INITIALIZER;

/*
 *	Find a client by clientlist id - must hold either a read or write lock
 *	before calling!
 */

static struct clientlist_t *clientlist_find_id(char *username, int len, void *id)
{
	struct clientlist_t *cl;
	uint32_t hash, idx;
	int i;
	
	hash = keyhash(username, len, 0);
	idx = hash;
	// "CLIENT_HEARD_BUCKETS" is 512..
	idx ^= (idx >> 16);
	idx ^= (idx >>  8);
	i = idx % CLIENTLIST_BUCKETS;
	
	DLOG("clientlist_find_id '%.*s' id %p bucket %d", len, username, id, i);
	for (cl = clientlist[i]; cl; cl = cl->next)
		if (cl->client_id == id) {
			DLOG("clientlist_find_id '%.*s' found %p", len, username, cl);
			return cl;
		}
	
	return NULL;
}

/*
 *	Check if given usename is logged in and validated.
 *	Return fd if found, -1 if not.
 *	Internal variant, no locking.
 */

static int check_if_validated_client(char *username, int len)
{
	struct clientlist_t *cl;
	uint32_t hash, idx;
	int i;
	
	hash = keyhash(username, len, 0);
	idx = hash;
	// "CLIENT_HEARD_BUCKETS" is 512..
	idx ^= (idx >> 16);
	idx ^= (idx >>  8);
	i = idx % CLIENTLIST_BUCKETS;
	
	DLOG("check_if_validated_client '%.*s': bucket %d", len, username, i);
	for (cl = clientlist[i]; cl; cl = cl->next) {
		if (cl->hash == hash && memcmp(username, cl->username, len) == 0
		  && cl->username_len == len && cl->validated) {
			DLOG("check_if_validated_client '%.*s' found validated, fd %d", cl->username_len, cl->username, cl->fd);
		  	return cl->fd;
		}
	}
	
	return -1;
}

/*
 *	Check if given usename is logged in and validated.
 *	Return fd if found, -1 if not.
 *	This is the external variant, with locking.
 */

int clientlist_check_if_validated_client(char *username, int len)
{
	int fd;
	
	rwl_rdlock(&clientlist_lock);
	
	fd = check_if_validated_client(username, len);
	
	rwl_rdunlock(&clientlist_lock);
	
	return fd;
}

/*
 *	Add a client to the client list
 */

int clientlist_add(struct client_t *c)
{
	struct clientlist_t *cl;
	int old_fd;
	uint32_t hash, idx;
	int i;
	
	hash = keyhash(c->username, c->username_len, 0);
	idx = hash;
	// "CLIENT_HEARD_BUCKETS" is 512..
	idx ^= (idx >> 16);
	idx ^= (idx >>  8);
	i = idx % CLIENTLIST_BUCKETS;
	
	DLOG("clientlist_add '%s': bucket %d", c->username, i);
	
	/* allocate and fill in */
	cl = hmalloc(sizeof(*cl));
	strncpy(cl->username, c->username, sizeof(cl->username));
	cl->username[sizeof(cl->username)-1] = 0;
	cl->username_len = c->username_len;
	cl->validated = c->validated;
	cl->fd = c->fd;
	cl->hash = hash;
	
	/* get lock for list and insert */
	rwl_wrlock(&clientlist_lock);
	
	old_fd = check_if_validated_client(c->username, c->username_len);
	
	if (clientlist[i])
		clientlist[i]->prevp = &cl->next;
	cl->next = clientlist[i];
	cl->prevp = &clientlist[i];
	cl->client_id = (void *)c;
	clientlist[i] = cl;
	rwl_wrunlock(&clientlist_lock);
	
	return old_fd;
}

/*
 *	Remove a client from the client list
 */

void clientlist_remove(struct client_t *c)
{
	struct clientlist_t *cl;
	
	DLOG("clientlist_remove '%s'", c->username);
	
	/* get lock for list, find, and remove */
	rwl_wrlock(&clientlist_lock);
	
	cl = clientlist_find_id(c->username, c->username_len, (void *)c);
	if (cl) {
		DLOG("clientlist_remove found '%s'", c->username);
		if (cl->next)
			cl->next->prevp = cl->prevp;
		*cl->prevp = cl->next;
		
		hfree(cl);
	}
	
	rwl_wrunlock(&clientlist_lock);
}

