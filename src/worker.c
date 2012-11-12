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
 *	worker.c: the worker thread
 */

#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <netinet/in.h>

#include "worker.h"

#include "config.h"
#include "hlog.h"
#include "hmalloc.h"
#include "login.h"
#include "uplink.h"
#include "incoming.h"
#include "outgoing.h"
#include "filter.h"
#include "dupecheck.h"
#include "clientlist.h"
#include "client_heard.h"
#include "cellmalloc.h"
#include "version.h"
#include "status.h"

time_t now;	/* current time, updated by the main thread, MAY be spun around by the simulator */
time_t tick;	/* real monotonous clock, may or may not be wallclock */

extern int ibuf_size;

struct worker_t *worker_threads;
struct client_udp_t *udppeers;	/* list of listening/receiving UDP peer sockets */

struct client_udp_t *udpclients;	/* list of listening/receiving UDP client sockets */
/* mutex to protect udpclient chain refcounts */
pthread_mutex_t udpclient_mutex = PTHREAD_MUTEX_INITIALIZER;

int workers_running;
int sock_write_expire  = 25;    /* 25 seconds, smaller than the 30-second dupe check window. */
int keepalive_interval = 20;    /* 20 seconds for individual socket, NOT all in sync! */
int keepalive_poll_freq = 2;	/* keepalive analysis scan interval */
int obuf_writes_threshold = 16;	/* This many writes per keepalive scan interval switch socket
				   output to buffered. */
int obuf_writes_threshold_hys = 6; /* Less than this, and switch back. */

/* global packet buffer */
rwlock_t pbuf_global_rwlock = RWL_INITIALIZER;
struct pbuf_t  *pbuf_global       = NULL;
struct pbuf_t **pbuf_global_prevp = &pbuf_global;
struct pbuf_t  *pbuf_global_dupe       = NULL;
struct pbuf_t **pbuf_global_dupe_prevp = &pbuf_global_dupe;


/* global inbound connects, and protocol traffic accounters */

struct portaccount_t inbound_connects = {
  .mutex    = PTHREAD_MUTEX_INITIALIZER,
  .refcount = 99,	/* Global static blocks have extra-high initial refcount */
};

/* global byte/packet counters per protocol */
struct portaccount_t client_connects_tcp = {
  .mutex    = PTHREAD_MUTEX_INITIALIZER,
  .refcount = 99,	/* Global static blocks have extra-high initial refcount */
};
struct portaccount_t client_connects_udp = {
  .mutex    = PTHREAD_MUTEX_INITIALIZER,
  .refcount = 99,	/* Global static blocks have extra-high initial refcount */
};
struct portaccount_t client_connects_sctp = {
  .mutex    = PTHREAD_MUTEX_INITIALIZER,
  .refcount = 99,	/* Global static blocks have extra-high initial refcount */
};

int worker_corepeer_client_count = 0;
struct client_t *worker_corepeer_clients[MAX_COREPEERS];

#ifndef _FOR_VALGRIND_
cellarena_t *client_cells;
#endif

/* clientlist collected at shutdown for live upgrade */
cJSON *worker_shutdown_clients = NULL;

static struct cJSON *worker_client_json(struct client_t *c, int liveup_info);

/* port accounters */
struct portaccount_t *port_accounter_alloc(void)
{
	struct portaccount_t *p;

	p = hmalloc(sizeof(*p));
	memset(p, 0, sizeof(*p));

	p->refcount = 1;
	pthread_mutex_init( & p->mutex, NULL );

	hlog(LOG_DEBUG, "new port_accounter %p", p);

	return p;
}

static void port_accounter_reject(struct portaccount_t *p)
{
	int i;
	if (!p) return;

	if ((i = pthread_mutex_lock(&p->mutex))) {
		hlog(LOG_ERR, "port_accounter_reject: could not lock portaccount: %s", strerror(i));
		return;
	}

	++ p->counter;
	
	if ((i = pthread_mutex_unlock(&p->mutex))) {
		hlog(LOG_ERR, "port_accounter_reject: could not unlock portaccount: %s", strerror(i));
		return;
	}
}

static void port_accounter_add(struct portaccount_t *p)
{
	int i;
	if (!p) return;

	if ((i = pthread_mutex_lock(&p->mutex))) {
		hlog(LOG_ERR, "port_accounter_add: could not lock portaccount: %s", strerror(i));
		return;
	}
	
	hlog(LOG_DEBUG, "port_accounter_add %p", p);

	++ p->refcount;
	++ p->counter;
	++ p->gauge;
	
	if (p->gauge > p->gauge_max)
		p->gauge_max = p->gauge;
	
	if ((i = pthread_mutex_unlock(&p->mutex))) {
		hlog(LOG_ERR, "port_accounter_add: could not unlock portaccount: %s", strerror(i));
		return;
	}
}

void port_accounter_drop(struct portaccount_t *p)
{
	int i, r;
	if (!p) return;

	if ((i = pthread_mutex_lock(&p->mutex))) {
		hlog(LOG_ERR, "port_accounter_drop: could not lock portaccount: %s", strerror(i));
		return;
	}

	-- p->refcount;
	-- p->gauge;

	r = p->refcount;

	if ((i = pthread_mutex_unlock(&p->mutex))) {
		hlog(LOG_ERR, "port_accounter_drop: could not unlock portaccount: %s", strerror(i));
		return;
	}

	hlog(LOG_DEBUG, "port_accounter_drop(%p) refcount=%d", p, r);

	if (r == 0) {
		/* Last reference is being destroyed */
		hfree(p);
	}
}

/*
 *	Global and port specific port usage counters
 */

void inbound_connects_account(const int add, struct portaccount_t *p) 
{	/* add == 2/3  --> UDP "client" socket drop/add, -1 --> rejected connect */
	int i;
	if (add < 2) {
		if ((i = pthread_mutex_lock(& inbound_connects.mutex ))) {
			hlog(LOG_ERR, "inbound_connects_account: could not lock inbound_connects: %s", strerror(i));
			return;
		}
		
		if (add == -1) {
			/* just increment connects, it was discarded */
			++ inbound_connects.counter;
		} else if (add) {
			++ inbound_connects.counter;
			++ inbound_connects.gauge;
			if (inbound_connects.gauge > inbound_connects.gauge_max)
				inbound_connects.gauge_max = inbound_connects.gauge;
		} else {
			-- inbound_connects.gauge;
		}

		if ((i = pthread_mutex_unlock(& inbound_connects.mutex )))
			hlog(LOG_ERR, "inbound_connects_account: could not unlock inbound_connects: %s", strerror(i));
	}
	
	if ( p ) {
		if ( add == -1 ) {
			port_accounter_reject(p);
		} else if ( add & 1 ) {
			port_accounter_add(p);
		} else {
			port_accounter_drop(p);
		}
	}

	// hlog( LOG_DEBUG, "inbound_connects_account(), count=%d gauge=%d max=%d", 
	//       inbound_connects.count, inbound_connects.gauge, inbound_connects.gauge_max );
}

/* object alloc/free */

void client_udp_free(struct client_udp_t *u)
{
	int i;

	if (!u) return;

	if ((i = pthread_mutex_lock(& udpclient_mutex ))) {
		hlog(LOG_ERR, "client_udp_free: could not lock udpclient_mutex: %s", strerror(i));
		return;
	}
	
	-- u->refcount;

	if (u)
		hlog(LOG_DEBUG, "client_udp_free %p port %d refcount now: %d", u, u->portnum, u->refcount);

	if ( // u->configured == 0 &&
	     u->refcount   == 0 ) {
		hlog(LOG_DEBUG, "client_udp_free %p port %d FREEING", u, u->portnum);
		/* Unchain, and destroy.. */
		if (u->next)
			u->next->prevp = u->prevp;
		*u->prevp = u->next;

		close(u->fd);

		hfree(u);
	}

	if ((i = pthread_mutex_unlock(& udpclient_mutex )))
		hlog(LOG_ERR, "client_udp_free: could not unlock udpclient_mutex: %s", strerror(i));
}

struct client_udp_t *client_udp_find(struct client_udp_t *root, const int af, const int portnum)
{
	struct client_udp_t *u;
	int i;

	if ((i = pthread_mutex_lock(& udpclient_mutex ))) {
		hlog(LOG_ERR, "client_udp_find: could not lock udpclient_mutex: %s", strerror(i));
		return NULL;
	}

	for (u = root ; u ; u = u->next ) {
		if (u->portnum == portnum && u->af == af) {
			++ u->refcount;
			break;
		}
	}

	//if (u)
	//	hlog(LOG_DEBUG, "client_udp_find %u port %d refcount now: %d", u, u->portnum, u->refcount);

	if ((i = pthread_mutex_unlock(& udpclient_mutex )))
		hlog(LOG_ERR, "client_udp_find: could not unlock udpclient_mutex: %s", strerror(i));

	return u;
}


struct client_udp_t *client_udp_alloc(struct client_udp_t **root, int fd, int portnum)
{
	struct client_udp_t *c;
	int i;
	
	/* TODO: hm, could maybe lock a bit later, just before adding to the udpclient list? */
	if ((i = pthread_mutex_lock(& udpclient_mutex ))) {
		hlog(LOG_ERR, "client_udp_alloc: could not lock udpclient_mutex: %s", strerror(i));
		return NULL;
	}

	c = hmalloc(sizeof(*c));
	c->configured = 1;
	c->polled     = 0;
	c->fd         = fd;
	c->refcount   = 1; /* One reference already on creation */
	c->portnum    = portnum;

	/* Add this to special list of UDP sockets */
	c->next  = *root;
	c->prevp = root;
	if (c->next)
		c->next->prevp = &c->next;
	*root = c;
	
	//hlog(LOG_DEBUG, "client_udp_alloc %u port %d refcount now: %d", c, c->portnum, c->refcount);

	if ((i = pthread_mutex_unlock(& udpclient_mutex )))
		hlog(LOG_ERR, "client_udp_alloc: could not lock udpclient_mutex: %s", strerror(i));

	return c;
}

/*
 *	Close and free all UDP core peers
 */

static void corepeer_close_all(struct worker_t *self)
{
	int i;
	struct client_t *c;
	
	for (i = 0; i < worker_corepeer_client_count; i++) {
		c = worker_corepeer_clients[i];
		client_close(self, c, CLIOK_PEERS_CLOSING);
		worker_corepeer_clients[i] = NULL;
	}
	
	worker_corepeer_client_count = 0;
}


/*
 *	set up cellmalloc for clients
 */

void client_init(void)
{
#ifndef _FOR_VALGRIND_
	client_cells  = cellinit( "clients",
				  sizeof(struct client_t),
				  __alignof__(struct client_t), CELLMALLOC_POLICY_FIFO,
				  4096 /* 4 MB at the time */, 0 /* minfree */ );
	/* 4 MB arena size -> about 100 clients per single arena
	   .. with 40 arenas -> 4000 clients max. */
#endif
}

struct client_t *client_alloc(void)
{
#ifndef _FOR_VALGRIND_
	struct client_t *c = cellmalloc(client_cells);
	if (!c) {
		hlog(LOG_ERR, "client_alloc: cellmalloc failed");
		return NULL;
	}
#else
	struct client_t *c = hmalloc(sizeof(*c));
#endif
	memset((void *)c, 0, sizeof(*c));
	c->fd = -1;
	c->state = CSTATE_INIT;

#ifdef FIXED_IOBUFS
	c->ibuf_size = sizeof(c->ibuf);
	c->obuf_size = sizeof(c->obuf);
#else
	c->ibuf_size = ibuf_size;
	c->obuf_size = obuf_size;

	c->ibuf      = hmalloc(c->ibuf_size);
	c->obuf      = hmalloc(c->obuf_size);
#endif

	return c;
}

void client_free(struct client_t *c)
{
	//hlog(LOG_DEBUG, "client_free %p: fd %d name %s addr_loc %s udpclient %p", c, c->fd, c->username, c->addr_loc, c->udpclient);
	
	if (c->fd >= 0)	 close(c->fd);
#ifndef FIXED_IOBUFS
	if (c->ibuf)     hfree(c->ibuf);
	if (c->obuf)     hfree(c->obuf);
	if (c->addr_rem) hfree(c->addr_rem);
	if (c->addr_loc) hfree(c->addr_loc);
	if (c->username) hfree(c->username);
	if (c->app_name) hfree(c->app_name);
	if (c->app_version) hfree(c->app_version);
#endif

	filter_free(c->posdefaultfilters);
	filter_free(c->negdefaultfilters);
	filter_free(c->posuserfilters);
	filter_free(c->neguserfilters);
	
	client_heard_free(c);

	client_udp_free(c->udpclient);
	clientlist_remove(c);

	memset(c, 0, sizeof(*c));

#ifndef _FOR_VALGRIND_
	cellfree(client_cells, c);
#else
	hfree(c);
#endif
}

/*
 *	Set up a pseudoclient for UDP and HTTP submitted packets
 */

struct client_t *pseudoclient_setup(int portnum)
{
	struct client_t *c;
	
	c = client_alloc();
	if (!c) {
		hlog(LOG_ERR, "pseudoclient_setup: client_alloc returned NULL");
		abort();
	}
	c->fd    = -1;
	c->portnum = portnum;
	c->state = CSTATE_CONNECTED;
	c->flags = CLFLAGS_INPORT|CLFLAGS_CLIENTONLY;
	c->validated = 1; // we will validate on every packet
	//c->portaccount = l->portaccount;
	c->keepalive = tick;
	c->connect_time = tick;
	c->last_read = tick;
	
	//hlog(LOG_DEBUG, "pseudoclient setup %p: fd %d name %s addr_loc %s udpclient %p", c, c->fd, c->username, c->addr_loc, c->udpclient);
	
	return c;
}

/*
 *	Pass a new client to a worker thread
 */

int pass_client_to_worker(struct worker_t *wc, struct client_t *c)
{
	int pe;
	
	hlog(LOG_DEBUG, "pass_client_to_worker: client on fd %d to thread %d with %d users", c->fd, wc->id, wc->client_count);

	if ((pe = pthread_mutex_lock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "pass_client_to_worker(): could not lock new_clients_mutex: %s", strerror(pe));
		return -1;
	}
	
	/* push the client in the worker's queue */
	c->next = NULL;
	
	if (wc->new_clients_last) {
		wc->new_clients_last->next = c;
		c->prevp = &wc->new_clients_last->next;
	} else {
		wc->new_clients = c;
		c->prevp = &wc->new_clients;
	}
	
	wc->new_clients_last = c;
	
	/* unlock the queue */
	if ((pe = pthread_mutex_unlock(&wc->new_clients_mutex))) {
		hlog(LOG_ERR, "pass_client_to_worker(): could not unlock new_clients_mutex: %s", strerror(pe));
		return -1;
	}
	
	return 0;
}

char *strsockaddr(const struct sockaddr *sa, const int addr_len)
{
	char eb[200], *s;
	char sbuf[20];
	union sockaddr_u su, *sup;

	sup = (union sockaddr_u *)sa;
#ifdef IN6_IS_ADDR_V4MAPPED
	if ( sa->sa_family == AF_INET6 && 
	     ( IN6_IS_ADDR_V4MAPPED(&(sup->si6.sin6_addr)) ||
	       IN6_IS_ADDR_V4COMPAT(&(sup->si6.sin6_addr)) ) ) {

		memset(&su, 0, sizeof(su));
		su.si.sin_family = AF_INET;
		su.si.sin_port   = sup->si6.sin6_port;
		memcpy(& su.si.sin_addr, &((uint32_t*)(&(sup->si6.sin6_addr)))[3], 4);
		sa = &su.sa;
		// sup = NULL;
		// hlog(LOG_DEBUG, "Translating v4 mapped/compat address..");
	}
#endif


	if ( sa->sa_family == AF_INET ) {
		eb[0] = 0;
		sbuf[0] = 0;

		getnameinfo( sa, addr_len,
			    eb, sizeof(eb), sbuf, sizeof(sbuf), NI_NUMERICHOST|NI_NUMERICSERV);
		s = eb + strlen(eb);

		sprintf(s, ":%s", sbuf);
	} else {
		/* presumption: IPv6 */
		eb[0] = '[';
		eb[1] = 0;
		sbuf[0] = 0;

		getnameinfo( sa, addr_len,
			    eb+1, sizeof(eb)-1, sbuf, sizeof(sbuf), NI_NUMERICHOST|NI_NUMERICSERV);
		s = eb + strlen(eb);

		sprintf(s, "]:%s", sbuf);
	}

	// if (!sup) hlog(LOG_DEBUG, "... to: %s", eb);

	return hstrdup(eb);
}

/*
 *	Generate a hexadecimal representation of the socket
 *	address, as used in the APRS-IS Q construct.
 */

char *hexsockaddr(const struct sockaddr *sa, const int addr_len)
{
	char eb[200];
	union sockaddr_u su, *sup;
	struct sockaddr_in *sa_in;
	uint8_t *in6;

	sup = (union sockaddr_u *)sa;
#ifdef IN6_IS_ADDR_V4MAPPED
	if ( sa->sa_family == AF_INET6 && 
	     ( IN6_IS_ADDR_V4MAPPED(&(sup->si6.sin6_addr)) ||
	       IN6_IS_ADDR_V4COMPAT(&(sup->si6.sin6_addr)) ) ) {

		memset(&su, 0, sizeof(su));
		su.si.sin_family = AF_INET;
		su.si.sin_port   = sup->si6.sin6_port;
		memcpy(& su.si.sin_addr, &((uint32_t*)(&(sup->si6.sin6_addr)))[3], 4);
		sa = &su.sa;
		// sup = NULL;
		// hlog(LOG_DEBUG, "Translating v4 mapped/compat address..");
	}
#endif
	
	/* As per the original implementation's example, the hex address is in upper case.
	 * For IPv6, there's simply more bytes in there.
	 */
	if ( sa->sa_family == AF_INET ) {
		sa_in = (struct sockaddr_in *)sa;
		sprintf(eb, "%02X%02X%02X%02X",
			sa_in->sin_addr.s_addr & 0xff,
			(sa_in->sin_addr.s_addr >> 8) & 0xff,
			(sa_in->sin_addr.s_addr >> 16) & 0xff,
			(sa_in->sin_addr.s_addr >> 24) & 0xff
			);
	} else {
		/* presumption: IPv6 */
		in6 = (uint8_t *)&sup->si6.sin6_addr.s6_addr;
		sprintf(eb, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
			in6[0], in6[1], in6[2], in6[3], in6[4], in6[5], in6[6], in6[7],
			in6[8], in6[9], in6[10], in6[11], in6[12], in6[13], in6[14], in6[15]);
	}

	// if (!sup) hlog(LOG_DEBUG, "... to: %s", eb);

	return hstrdup(eb);
}

void clientaccount_add(struct client_t *c, int l4proto, int rxbytes, int rxpackets, int txbytes, int txpackets, int rxerr, int rxdupes)
{
	struct portaccount_t *pa = NULL;
	int rxdrops = 0;
	
	if (rxerr < 0) {
		rxdrops = 1;
		if (rxerr < INERR_MIN)
			rxerr = INERR_UNKNOWN; /* which is 0 */
		rxerr *= -1;
	}
	
	/* worker local accounters do not need locks */
	c->localaccount.rxbytes   += rxbytes;
	c->localaccount.txbytes   += txbytes;
	c->localaccount.rxpackets += rxpackets;
	c->localaccount.txpackets += txpackets;
	c->localaccount.rxdupes   += rxdupes;
	if (rxdrops) {
		c->localaccount.rxdrops += 1;
		c->localaccount.rxerrs[rxerr] += 1;
	}
	
	if (l4proto == IPPROTO_UDP && c->udpclient && c->udpclient->portaccount) {
		pa = c->udpclient->portaccount;
	} else if (c->portaccount) {
		pa = c->portaccount;
	}
	
	if (pa) {
#ifdef HAVE_SYNC_FETCH_AND_ADD
		__sync_fetch_and_add(&pa->rxbytes, rxbytes);
		__sync_fetch_and_add(&pa->txbytes, txbytes);
		__sync_fetch_and_add(&pa->rxpackets, rxpackets);
		__sync_fetch_and_add(&pa->txpackets, txpackets);
		__sync_fetch_and_add(&pa->rxdupes, rxdupes);
		if (rxdrops) {
			__sync_fetch_and_add(&pa->rxdrops, 1);
			__sync_fetch_and_add(&pa->rxerrs[rxerr], 1);
		}
#else
		// FIXME: MUTEX !! -- this may or may not need locks..
		pa->rxbytes   += rxbytes;
		pa->txbytes   += txbytes;
		pa->rxpackets += rxpackets;
		pa->txpackets += txpackets;
		pa->rxdupes   += rxdupes;
		if (rxdrops) {
			pa->rxdrops += 1;
			pa->rxerrs[rxerr] += 1;
		}
#endif
	}

	if (l4proto == IPPROTO_UDP) {
#ifdef HAVE_SYNC_FETCH_AND_ADD
		__sync_fetch_and_add(&client_connects_udp.rxbytes, rxbytes);
		__sync_fetch_and_add(&client_connects_udp.txbytes, txbytes);
		__sync_fetch_and_add(&client_connects_udp.rxpackets, rxpackets);
		__sync_fetch_and_add(&client_connects_udp.txpackets, txpackets);
		if (rxdrops) {
			__sync_fetch_and_add(&client_connects_udp.rxdrops, 1);
			__sync_fetch_and_add(&client_connects_udp.rxerrs[rxerr], 1);
		}
#else
		// FIXME: MUTEX !! -- this may or may not need locks..
		client_connects_udp.rxbytes   += rxbytes;
		client_connects_udp.txbytes   += txbytes;
		client_connects_udp.rxpackets += rxpackets;
		client_connects_udp.txpackets += txpackets;
		if (rxdrops) {
			client_connects_udp.rxdrops += 1;
			client_connects_udp.rxerrs[rxerr] += 1;
		}
#endif
	} else {
#ifdef HAVE_SYNC_FETCH_AND_ADD
		__sync_fetch_and_add(&client_connects_tcp.rxbytes, rxbytes);
		__sync_fetch_and_add(&client_connects_tcp.txbytes, txbytes);
		__sync_fetch_and_add(&client_connects_tcp.rxpackets, rxpackets);
		__sync_fetch_and_add(&client_connects_tcp.txpackets, txpackets);
		if (rxdrops) {
			__sync_fetch_and_add(&client_connects_tcp.rxdrops, 1);
			__sync_fetch_and_add(&client_connects_tcp.rxerrs[rxerr], 1);
		}
#else
		// FIXME: MUTEX !! -- this may or may not need locks..
		client_connects_tcp.rxbytes   += rxbytes;
		client_connects_tcp.txbytes   += txbytes;
		client_connects_tcp.rxpackets += rxpackets;
		client_connects_tcp.txpackets += txpackets;
		if (rxdrops) {
			client_connects_tcp.rxdrops += 1;
			client_connects_tcp.rxerrs[rxerr] += 1;
		}
#endif
	}
}

/*
 *	close and forget a client connection
 */

void client_close(struct worker_t *self, struct client_t *c, int errnum)
{
	int pe;
	
	hlog( LOG_INFO, "%s %s (%s) closed after %d s: %s, tx/rx %lld/%lld bytes %lld/%lld pkts, dropped %lld, fd %d, worker %d%s%s%s%s",
	      ( (c->flags & CLFLAGS_UPLINKPORT)
			  ? ((c->state == CSTATE_COREPEER) ? "Peer" : "Uplink") : "Client" ),
			  	c->addr_rem,
			  	((c->username[0]) ? c->username : "?"),
			  	tick - c->connect_time,
			  	((errnum >= 0) ? strerror(errnum) : aprsc_strerror(errnum)),
			  	c->localaccount.txbytes,
			  	c->localaccount.rxbytes,
			  	c->localaccount.txpackets,
			  	c->localaccount.rxpackets,
			  	c->localaccount.rxdrops,
			  	c->fd,
			  	self->id,
			  	(c->app_name) ? " app " : "",
			  	(c->app_name) ? c->app_name : "",
			  	(c->app_version) ? " ver " : "",
			  	(c->app_version) ? c->app_version : ""
			  	);
			  	
	if (c->localaccount.rxdrops) {
		char s[256] = "";
		int p = 0;
		int i;
		
		for (i = 0; i < INERR_BUCKETS; i++) {
			if (c->localaccount.rxerrs[i]) {
				p += snprintf(s+p, 256-p-2, "%s%s %lld",
					(p == 0) ? "" : ", ",
					inerr_labels[i], c->localaccount.rxerrs[i]);
			}
		}
		
		hlog(LOG_INFO, "%s (%s) rx drops: %s",
			c->addr_rem, c->username, s);
	}

	/* remove from polling list */
	if (c->xfd) {
		hlog(LOG_DEBUG, "client_close: xpoll_remove %p fd %d", c->xfd, c->xfd->fd);
		xpoll_remove(&self->xp, c->xfd);
	}
	
	/* close */
	if (c->fd >= 0) {
		close(c->fd);
	}

	c->fd = -1;
	
	/* If this thread already owns the mutex (we're closing the socket
	 * while traversing the thread's client list), FreeBSD's mutex lock
	 * will fail with EDEADLK:
	 *
	 * 2012/08/15 10:03:34.065703 aprsc[41159:800f12fc0] ERROR:
	 * client_close(worker 1): could not lock clients_mutex: Resource deadlock
	 * avoided
	 *
	 * If this happens, let's remember we've locked the mutex earlier,
	 * and let's not unlock it either.
	 *
	 * The current example of this is when collect_new_clients() sends
	 * the "# aprsc VERSION" login string to new clients. The client_printf()
	 * may fail if the client connects and disconnects very quickly, and
	 * this will cause it to client_close() during the collection loop.
	 */
	if ((pe = pthread_mutex_lock(&self->clients_mutex)) && pe != EDEADLK) {
		hlog(LOG_ERR, "client_close(worker %d): could not lock clients_mutex: %s", self->id, strerror(pe));
		return;
	}
	if (pe == EDEADLK) {
		hlog(LOG_ERR, "client_close(worker %d): could not lock clients_mutex (ignoring): %s", self->id, strerror(pe));
	}
	
	/* link the list together over this node */
	if (c->next)
		c->next->prevp = c->prevp;
	*c->prevp = c->next;
	
	/* link the classified clients list together over this node */
	if (c->class_prevp) {
		*c->class_prevp = c->class_next;
		if (c->class_next)
			c->class_next->class_prevp = c->class_prevp;
	}

	/* If this happens to be the uplink, tell the uplink connection
	 * setup module that the connection has gone away.
	 */
	if (c->flags & CLFLAGS_UPLINKPORT && c->state != CSTATE_COREPEER)
		uplink_close(c, errnum);
	
	if (c->portaccount) {
		/* If port accounting is done, handle population accounting... */
		hlog(LOG_DEBUG, "client_close dropping inbound_connects_account %p", c->portaccount);
		inbound_connects_account(0, c->portaccount);
		c->portaccount = NULL;
	} else {
		hlog(LOG_DEBUG, "client_close: has no portaccount");
	}
	
	if (c->udp_port && c->udpclient->portaccount) {
		inbound_connects_account(2, c->udpclient->portaccount); /* udp client count goes down */
	}

	/* free it up */
	client_free(c);
	
	/* if we held the lock before locking, let's not unlock it either */
	if (pe == EDEADLK) {
		hlog(LOG_ERR, "client_close(worker %d): closed client while holding clients_mutex", self->id);
	} else {
		if ((pe = pthread_mutex_unlock(&self->clients_mutex))) {
			hlog(LOG_ERR, "client_close(worker %d): could not unlock clients_mutex: %s", self->id, strerror(pe));
			exit(1);
		}
	}
	
	/* reduce client counter */
	self->client_count--;
}

/*
 *	write data to a client (well, at least put it in the output buffer)
 *	(this is also used with len=0 to flush current buffer)
 */

int client_write(struct worker_t *self, struct client_t *c, char *p, int len)
{
	int i, e;

	/* Count the number of writes towards this client,  the keepalive
	   manager monitors this counter to determine if the socket should be
	   kept in BUFFERED mode, or written immediately every time.
	   Buffer flushing is done every keepalive_poll_freq (2) seconds.
	*/
	c->obuf_writes++;

	//hlog(LOG_DEBUG, "client_write: %*s\n", len, p);

	if (c->udp_port && c->udpclient && len > 0 && *p != '#') {
		/* Every packet ends with CRLF, but they are not sent over UDP ! */
		/* Existing system doesn't send keepalives via UDP.. */
		i = sendto( c->udpclient->fd, p, len-2, MSG_DONTWAIT,
			    &c->udpaddr.sa, c->udpaddrlen );
			    
		if (i < 0) {
			hlog(LOG_ERR, "UDP transmit error to %s udp port %d: %s",
				c->addr_rem, c->udp_port, strerror(errno));
		} else if (i != len -2) {
			hlog(LOG_ERR, "UDP transmit incomplete to %s udp port %d: wrote %d of %d bytes, errno: %s",
				c->addr_rem, c->udp_port, i, len-2, strerror(errno));
		}

		// hlog( LOG_DEBUG, "UDP from %d to client port %d, sendto rc=%d", c->udpclient->portnum, c->udp_port, i );

		if (i > 0)
			clientaccount_add( c, IPPROTO_UDP, 0, 0, i, 0, 0, 0);
			
		return i;
	}
	
	// Should this return happen already in the previous block?
	// Is this a leftover from times before UDP support?
	if (c->state == CSTATE_UDP || c->state == CSTATE_COREPEER)
		return 0;

	if (len > 0) {
		/* Here, we only increment the bytes counter. Packets counter
		 * will be incremented only when we actually transmit a packet
		 * instead of a keepalive.
		 */
		clientaccount_add( c, IPPROTO_TCP, 0, 0, len, 0, 0, 0);
	}
	
	if (c->obuf_end + len > c->obuf_size) {
		/* Oops, cannot append the data to the output buffer.
		 * Check if we can make space for it by moving data
		 * towards the beginning of the buffer?
		 */
		if (len > c->obuf_size - (c->obuf_end - c->obuf_start)) {
			/* Oh crap, the data will not fit even if we move stuff.
			   Lets try writing.. */
		write_retry:;
			i = write(c->fd, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
			e = errno;
			if (i < 0 && (e == EINTR)) {
				// retrying..
				if (c->obuf_start > 0)
					memmove((void *)c->obuf, (void *)c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
				c->obuf_end  -= c->obuf_start;
				c->obuf_start = 0;
				goto write_retry;
			}
			if (i < 0 && e == EPIPE) {
				/* Remote socket closed.. */
				hlog(LOG_DEBUG, "client_write(%s) fails/2 EPIPE; %s", c->addr_rem, strerror(e));
				// WARNING: This also destroys the client object!
				client_close(self, c, e);
				return -9;
			}
			if (i < 0 && (e == EAGAIN || e == EWOULDBLOCK)) {
				/* Kernel's transmit buffer is full. They're pretty
				 * big, so we'll assume the client is dead and disconnect.
				 * Use "buffer overflow" as error message.
				 */
				hlog(LOG_DEBUG, "client_write(%s) fails/2a; disconnecting; %s", c->addr_rem, strerror(e));
				client_close(self, c, CLIERR_OUTPUT_BUFFER_FULL);
				return -10;
			}
			if (i < 0) {
				hlog(LOG_DEBUG, "client_write(%s) fails/2b; disconnecting; %s", c->addr_rem, strerror(e));
				client_close(self, c, e);
				return -11;
			}
			if (i > 0) {
				c->obuf_start += i;
				c->obuf_wtime = tick;
			}
			/* Is it still out of space ? */
			if (len > c->obuf_size - (c->obuf_end - c->obuf_start)) {
				/* Oh crap, the data will still not fit! */
				hlog(LOG_DEBUG, "client_write(%s) can not fit new data in buffer; disconnecting", c->addr_rem);
				client_close(self, c, CLIERR_OUTPUT_BUFFER_FULL);
				return -12;
			}
		}
		/* okay, move stuff to the beginning to make space in the end */
		if (c->obuf_start > 0)
			memmove((void *)c->obuf, (void *)c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
		c->obuf_end  -= c->obuf_start;
		c->obuf_start = 0;
	}
	
	/* copy data to the output buffer */
	if (len > 0)
		memcpy((void *)c->obuf + c->obuf_end, p, len);
	c->obuf_end += len;
	
	/* Is it over the flush size ? */
	if (c->obuf_end > c->obuf_flushsize || ((len == 0) && (c->obuf_end > c->obuf_start))) {
		/*if (c->obuf_end > c->obuf_flushsize)
		 *	hlog(LOG_DEBUG, "flushing fd %d since obuf_end %d > %d", c->fd, c->obuf_end, c->obuf_flushsize);
		 */
	write_retry_2:;
		i = write(c->fd, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
		e = errno;
		if (i < 0 && e == EINTR)
			goto write_retry_2;
		if (i < 0 && e == EPIPE) {
			/* Remote socket closed.. */
			hlog(LOG_DEBUG, "client_write(%s) fails/2 EPIPE; disconnecting; %s", c->addr_rem, strerror(e));
			// WARNING: This also destroys the client object!
			client_close(self, c, e);
			return -9;
		}
		if (i < 0 && (e == EAGAIN || e == EWOULDBLOCK)) {
			/* Kernel's transmit buffer is full (per-socket or some more global resource).
			 * This happens even with small amounts of data in real world:
			 * aprsc INFO: Client xx.yy.zz.ff:22823 (XXXXX) closed after 1 s:
			 *    Resource temporarily unavailable, tx/rx 735/51 bytes 8/0 pkts,
			 *    dropped 0, fd 59, worker 1 app aprx ver 2.00
			 */
			hlog(LOG_DEBUG, "client_write(%s) fails/2c; %s", c->addr_rem, strerror(e));
			return -1;
		}
		if (i < 0 && len != 0) {
			hlog(LOG_DEBUG, "client_write(%s) fails/2d; disconnecting; %s", c->addr_rem, strerror(e));
			client_close(self, c, e);
			return -11;
		}
		if (i > 0) {
			//hlog(LOG_DEBUG, "client_write(%s) wrote %d", c->addr_rem, i);
			c->obuf_start += i;
			c->obuf_wtime = tick;
		}
	}
	
	/* All done ? */
	if (c->obuf_start >= c->obuf_end) {
		//hlog(LOG_DEBUG, "client_write(%s) obuf empty", c->addr_rem);
		c->obuf_start = 0;
		c->obuf_end   = 0;
		return len;
	}

	/* tell the poller that we have outgoing data */
	xpoll_outgoing(&self->xp, c->xfd, 1);
	
	return len; 
}

/*
 *	printf to a client
 */

int client_printf(struct worker_t *self, struct client_t *c, const char *fmt, ...)
{
	va_list args;
	char s[PACKETLEN_MAX];
	int i;
	
	va_start(args, fmt);
	i = vsnprintf(s, PACKETLEN_MAX, fmt, args);
	va_end(args);
	
	if (i < 0 || i >= PACKETLEN_MAX) {
		hlog(LOG_ERR, "client_printf vsnprintf failed to %s: '%s'", c->addr_rem, fmt);
		return -1;
	}
	
	return client_write(self, c, s, i);
}

/*
 *	tell the client once that it has bad filter definition
 */
int client_bad_filter_notify(struct worker_t *self, struct client_t *c, const char *filt)
{
	if (!c->warned) {
		c->warned = 1;
		return client_printf(self, c, "# Warning: Bad filter: %s\r\n", filt);
	}
	return 0;
}

/*
 *	Receive UDP packets from a core peer
 */

static int handle_corepeer_readable(struct worker_t *self, struct client_t *c)
{
	struct client_t *rc = NULL; // real client
	union sockaddr_u addr;
	socklen_t addrlen;
	int i;
	int r;
	char *addrs;
	
	addrlen = sizeof(addr);
	r = recvfrom( c->udpclient->fd, c->ibuf, c->ibuf_size-1,
		MSG_DONTWAIT|MSG_TRUNC, (struct sockaddr *)&addr, &addrlen );
	
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0; /* D'oh..  return again latter */

		hlog( LOG_DEBUG, "recv: Error from corepeer UDP socket fd %d (%s): %s",
			c->udpclient->fd, c->addr_rem, strerror(errno));
		
		return 0;
	}
	
	if (r == 0) {
		hlog( LOG_DEBUG, "recv: EOF from corepeer UDP socket fd %d (%s)",
			c->udpclient->fd, c->addr_rem);
		return 0;
	}
	
	// Figure the correct client/peer based on the remote IP address.
	for (i = 0; i < worker_corepeer_client_count; i++) {
		rc = worker_corepeer_clients[i];
		
		if (rc->udpaddrlen != addrlen)
			continue;
		if (rc->udpaddr.sa.sa_family != addr.sa.sa_family)
			continue;
			
		if (addr.sa.sa_family == AF_INET) {
			if (memcmp(&rc->udpaddr.si.sin_addr, &addr.si.sin_addr, sizeof(addr.si.sin_addr)) != 0)
				continue;
			if (rc->udpaddr.si.sin_port != addr.si.sin_port)
				continue;
				
			break;
		} else if (addr.sa.sa_family == AF_INET6) {
			if (memcmp(&rc->udpaddr.si6.sin6_addr, &addr.si6.sin6_addr, sizeof(addr.si6.sin6_addr)) != 0)
				continue;
			if (rc->udpaddr.si6.sin6_port != addr.si6.sin6_port)
				continue;
				
			break;
		}
	}
	
	if (i == worker_corepeer_client_count || !rc) {
		addrs = strsockaddr(&addr.sa, addrlen);
		hlog(LOG_INFO, "recv: Received UDP peergroup packet from unknown peer address %s: %*s", addrs, r, c->ibuf);
		hfree(addrs);
		return 0;
	}
	
	/*
	addrs = strsockaddr(&addr.sa, addrlen);
	hlog(LOG_DEBUG, "worker thread passing UDP packet from %s to handler: %*s", addrs, r, c->ibuf);
	hfree(addrs);
	*/
	clientaccount_add( rc, IPPROTO_UDP, r, 0, 0, 0, 0, 0); /* Account byte count. incoming_handler() will account packets. */
	rc->last_read = tick;
	
	/* Ignore CRs and LFs in UDP input packet - the current core peer system puts 1 APRS packet in each
	 * UDP frame.
	 * TODO: consider processing multiple packets from an UDP frame, split up by CRLF.
	 */
	for (i = 0; i < r; i++) {
		if (c->ibuf[i] == '\r' || c->ibuf[i] == '\n') {
			r = i;
			break;
		}
	}
	
	c->handler(self, rc, IPPROTO_UDP, c->ibuf, r);
	
	return 0;
}

/*
 *	handle an event on an fd
 */

static int handle_client_readable(struct worker_t *self, struct client_t *c)
{
	int r;
	char *s;
	char *ibuf_end;
	char *row_start;
	
	/* Worker 0 takes care of reading corepeer UDP sockets and processes the incoming packets. */
	if (c->state == CSTATE_COREPEER && c->udpclient) {
		return handle_corepeer_readable(self, c);
	}
	
	if (c->fd < 0) {
		hlog(LOG_DEBUG, "socket no longer alive, closing (%s)", c->fd, c->addr_rem);
		client_close(self, c, CLIERR_FD_NUM_INVALID);
		return -1;
	}
	
	r = read(c->fd, c->ibuf + c->ibuf_end, c->ibuf_size - c->ibuf_end - 1);
	
	if (r == 0) {
		hlog( LOG_DEBUG, "read: EOF from socket fd %d (%s @ %s)",
		      c->fd, c->addr_rem, c->addr_loc );
		client_close(self, c, CLIERR_EOF);
		return -1;
	}
	
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0; /* D'oh..  return again later */

		hlog( LOG_DEBUG, "read: Error from socket fd %d (%s): %s",
		      c->fd, c->addr_rem, strerror(errno));
		hlog( LOG_DEBUG, " .. ibuf=%p  ibuf_end=%d  ibuf_size=%d",
		      c->ibuf, c->ibuf_end, c->ibuf_size-c->ibuf_end-1);
		client_close(self, c, errno);
		return -1;
	}

	clientaccount_add(c, IPPROTO_TCP, r, 0, 0, 0, 0, 0); /* Number of packets is now unknown,
					     byte count is collected.
					     The incoming_handler() will account
					     packets. */

	c->ibuf_end += r;
	// hlog( LOG_DEBUG, "read: %d bytes from client fd %d (%s) - %d in ibuf",
	//       r, c->fd, c->addr_rem, c->ibuf_end);
	
	/* parse out rows ending in CR and/or LF and pass them to the handler
	 * without the CRLF (we accept either CR or LF or both, but make sure
	 * to always output CRLF
	 */
	ibuf_end = c->ibuf + c->ibuf_end;
	row_start = c->ibuf;
	c->last_read = tick; /* not simulated time */

	for (s = c->ibuf; s < ibuf_end; s++) {
		if (*s == '\r' || *s == '\n') {
			/* found EOL */
			if (s - row_start > 0) {
			  // int ch = *s;
			  // *s = 0;
			  // hlog( LOG_DEBUG, "got: %s\n", row_start );
			  // *s = ch;

			  /* NOTE: handler call CAN destroy the c-> object ! */
			  if (c->handler(self, c, IPPROTO_TCP, row_start, s - row_start) < 0)
			    return -1;
			}
			/* skip the first, just-found part of EOL, which might have been
			 * NULled by the login handler (TODO: make it not NUL it) */
			s++;
			/* skip the rest of EOL */
			while (s < ibuf_end && (*s == '\r' || *s == '\n'))
				s++;
			row_start = s;
		}
	}
	
	if (row_start >= ibuf_end) {
		/* ok, we processed the whole buffer, just mark it empty */
		c->ibuf_end = 0;
	} else if (row_start != c->ibuf) {
		/* ok, we found data... move the buffer contents to the beginning */
		c->ibuf_end = ibuf_end - row_start;
		memmove(c->ibuf, row_start, c->ibuf_end);
	}
	
	return 0;
}

static int handle_client_writeable(struct worker_t *self, struct client_t *c)
{
	int r;
	
	if (c->obuf_start == c->obuf_end) {
		/* there is nothing to write any more */
		//hlog(LOG_DEBUG, "writable: nothing to write on fd %d (%s)", c->fd, c->addr_rem);
		xpoll_outgoing(&self->xp, c->xfd, 0);
		c->obuf_start = c->obuf_end = 0;
		return 0;
	}
	
	r = write(c->fd, c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
	if (r < 0) {
		if (errno == EINTR || errno == EAGAIN) {
			hlog(LOG_DEBUG, "writable: Would block fd %d (%s): %s", c->fd, c->addr_rem, strerror(errno));
			return 0;
		}
		
		hlog(LOG_DEBUG, "writable: Error from socket fd %d (%s): %s", c->fd, c->addr_rem, strerror(errno));
		client_close(self, c, errno);
		return -1;
	}
	
	c->obuf_start += r;
	//hlog(LOG_DEBUG, "writable: %d bytes to socket fd %d (%s) - %d in obuf", r, c->fd, c->addr_rem, c->obuf_end - c->obuf_start);
	
	if (c->obuf_start == c->obuf_end) {
		xpoll_outgoing(&self->xp, c->xfd, 0);
		c->obuf_start = c->obuf_end = 0;
	}
	
	return 0;
}

static int handle_client_event(struct xpoll_t *xp, struct xpoll_fd_t *xfd)
{
	struct worker_t *self = (struct worker_t *)xp->tp;
	struct client_t *c    = (struct client_t *)xfd->p;
	
	//hlog(LOG_DEBUG, "handle_client_event(%d): %d", xfd->fd, xfd->result);

	if (xfd->result & XP_OUT) {  /* priorize doing output */
		/* ah, the client is writable */
		if (handle_client_writeable(self, c) < 0)
			return 0;
	}

	if (xfd->result & XP_IN) {  /* .. before doing input */
		/* ok, read */
		if (handle_client_readable(self, c) < 0)
			return 0;
	}
	
	return 0;
}

/*
 *	move new clients from the new clients queue to the worker thread
 */

static void collect_new_clients(struct worker_t *self)
{
	int pe, n, i;
	struct client_t *new_clients, *c;
	
	/* lock the queue */
	if ((pe = pthread_mutex_lock(&self->new_clients_mutex))) {
		hlog(LOG_ERR, "collect_new_clients(worker %d): could not lock new_clients_mutex: %s", self->id, strerror(pe));
		return;
	}
	
	/* quickly grab the new clients to a local variable */
	new_clients = self->new_clients;
	self->new_clients = NULL;
	self->new_clients_last = NULL;
	
	/* unlock */
	if ((pe = pthread_mutex_unlock(&self->new_clients_mutex))) {
		hlog(LOG_ERR, "collect_new_clients(worker %d): could not unlock new_clients_mutex: %s", self->id, strerror(pe));
		/* we'd be going to deadlock here... */
		exit(1);
	}
	
	if ((pe = pthread_mutex_lock(&self->clients_mutex))) {
		hlog(LOG_ERR, "collect_new_clients(worker %d): could not lock clients_mutex: %s", self->id, strerror(pe));
		return;
	}
	
	/* move the new clients to the thread local client list */
	n = self->xp.pollfd_used;
	i = 0;
	while (new_clients) {
		i++;
		c = new_clients;
		new_clients = c->next;
		
		if (c->fd != -2) {
			self->client_count++;
			// hlog(LOG_DEBUG, "collect_new_clients(worker %d): got client fd %d", self->id, c->fd);
			c->next = self->clients;
			if (c->next)
				c->next->prevp = &c->next;
			self->clients = c;
			c->prevp = &self->clients;
			
			struct client_t *class_next = NULL;
			struct client_t **class_prevp = NULL;
			if (c->flags & CLFLAGS_PORT_RO) {
				hlog(LOG_DEBUG, "collect_new_clients(worker %d): client fd %d classified readonly", self->id, c->fd);
				class_next = self->clients_ro;
				class_prevp = &self->clients_ro;
			} else if (c->state == CSTATE_COREPEER || (c->flags & CLFLAGS_UPLINKPORT)) {
				hlog(LOG_DEBUG, "collect_new_clients(worker %d): client fd %d classified upstream/peer", self->id, c->fd);
				class_next = self->clients_ups;
				class_prevp = &self->clients_ups;
			} else if (c->flags & CLFLAGS_DUPEFEED) {
				hlog(LOG_DEBUG, "collect_new_clients(worker %d): client fd %d classified dupefeed", self->id, c->fd);
				class_next = self->clients_dupe;
				class_prevp = &self->clients_dupe;
			} else if (c->flags & CLFLAGS_INPORT) {
				hlog(LOG_DEBUG, "collect_new_clients(worker %d): client fd %d classified other", self->id, c->fd);
				class_next = self->clients_other;
				class_prevp = &self->clients_other;
			} else {
				hlog(LOG_ERR, "collect_new_clients(worker %d): client fd %d NOT CLASSIFIED - will not get packets", self->id, c->fd);
			}
			c->class_next = class_next;
			if (class_next)
				class_next->class_prevp = &c->class_next;
			*class_prevp = c;
			c->class_prevp = class_prevp;
		}
		
		/* If the new client is an UDP core peer, we will add it's FD to the
		 * polling list, but only once. There is only a single listener socket
		 * for a single peer group.
		 */
		if (c->state == CSTATE_COREPEER) {
			if (c->fd == -2) {
				/* corepeer reconfig flag */
				hlog(LOG_DEBUG, "collect_new_clients(worker %d): closing all existing peergroup peers", self->id);
				corepeer_close_all(self);
				client_free(c);
				i--; /* don't count it in */
				continue;
			}
			
			/* add to corepeer client list and polling list */
			hlog(LOG_DEBUG, "collect_new_clients(worker %d): got core peergroup peer, UDP fd %d", self->id, c->udpclient->fd);
			
			if (worker_corepeer_client_count == MAX_COREPEERS) {
				hlog(LOG_ERR, "worker: Too many core peergroup peers (max %d)", MAX_COREPEERS);
				exit(1);
			}
			
			/* build a static array of clients, for quick searching based on address */
			worker_corepeer_clients[worker_corepeer_client_count] = c;
			worker_corepeer_client_count++;
			
			if (!c->udpclient->polled) {
				c->udpclient->polled = 1;
				c->xfd = xpoll_add(&self->xp, c->udpclient->fd, (void *)c);
				hlog(LOG_DEBUG, "collect_new_clients(worker %d): starting poll for UDP fd %d xfd %p", self->id, c->udpclient->fd, c->xfd);
			}
			
			continue;
		}
		
		/* add to polling list */
		c->xfd = xpoll_add(&self->xp, c->fd, (void *)c);
		hlog(LOG_DEBUG, "collect_new_clients(worker %d): added fd %d to polling list, xfd %p", self->id, c->fd, c->xfd);
		if (!c->xfd) {
			/* ouch, out of xfd space */
			shutdown(c->fd, SHUT_RDWR);
			continue;
		}
		/* The new client may end up destroyed right away, never mind it here.
		 * We will notice it later and discard the client.
		 */
		
		/* According to http://www.aprs-is.net/ServerDesign.aspx, the server must
		 * initially transmit it's software name and version string.
		 * In case of a live upgrade, this should maybe be skipped, but
		 * I'll leave it in for now.
		 */
		client_printf(self, c, "# %s\r\n", verstr_aprsis);
		
		/* If the write failed immediately, c is already invalid at this point. Don't touch it. */
	}
	
	if ((pe = pthread_mutex_unlock(&self->clients_mutex))) {
		hlog(LOG_ERR, "collect_new_clients(worker %d): could not unlock clients_mutex: %s", self->id, strerror(pe));
		exit(1);
	}
	
	hlog( LOG_DEBUG, "Worker %d accepted %d new clients, %d new connections, now total %d clients",
	      self->id, i, self->xp.pollfd_used - n, self->client_count );
}

/* 
 *	Send keepalives to client sockets, run this once a second
 *	This watches also obuf_wtime becoming too old, and also about
 *	the number of writes on socket in previous run interval to
 *	auto-adjust socket buffering mode.
 */
static void send_keepalives(struct worker_t *self)
{
	struct client_t *c, *cnext;
	struct tm t;
	char buf[230], *s;
	int len0, len, rc;
	static const char *monthname[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
	time_t w_expire    = tick - sock_write_expire;
	time_t w_keepalive = tick - keepalive_interval;

	// Example message:
	// # javAPRSSrvr 3.12b12 1 Mar 2008 15:11:20 GMT T2FINLAND 85.188.1.32:14580

	sprintf(buf, "# %.40s ", verstr_aprsis);
	s = buf + strlen(buf);

	memset(&t, 0, sizeof(t));
	gmtime_r(&now, &t);
	// s += strftime(s, 40, "%d %b %Y %T GMT", &t);
	// However that depends upon LOCALE, thus following:
	s += sprintf(s, "%d %s %d %02d:%02d:%02d GMT",
		     t.tm_mday, monthname[t.tm_mon], t.tm_year + 1900,
		     t.tm_hour, t.tm_min, t.tm_sec);

	s += sprintf(s, " %s ", serverid);

	len0 = (s - buf);

	for (c = self->clients; (c); c = cnext) {
		// the  c  may get destroyed from underneath of ourselves!
		cnext = c->next;

		/* No keepalives on PEER links.. */
		if ( c->state == CSTATE_COREPEER )
			continue;
		
		/* Is it time for keepalive? Also send a keepalive if clock jumped backwards. */
		if ((c->keepalive <= tick && c->obuf_wtime < w_keepalive)
		    || (c->keepalive > tick + keepalive_interval)) {
			int flushlevel = c->obuf_flushsize;
			c->keepalive = tick + keepalive_interval;

			len = len0 + sprintf(s, "%s\r\n", c->addr_loc);

			c->obuf_flushsize = 0;
			/* Write out immediately */
			rc = client_write(self, c, buf, len);
			if (rc < -2) continue; // destroyed
			c->obuf_flushsize = flushlevel;
		} else {
			/* just fush if there was anything to write */
			rc = client_write(self, c, buf, 0);
			if (rc < -2) continue; // destroyed..
		}
		
		/* Check for input timeouts. These will currently also kick in if the
		 * real-time clock jumps backwards for some reason.
		 */
		if (c->flags & CLFLAGS_INPORT) {
			if (c->state != CSTATE_CONNECTED) {
				if (c->connect_time <= tick - client_login_timeout) {
					hlog(LOG_DEBUG, "%s: Closing client fd %d due to login timeout (%d s)",
					      c->addr_rem, c->fd, client_login_timeout);
					client_close(self, c, CLIERR_LOGIN_TIMEOUT);
					continue;
				}
			} else {
				if (c->last_read <= tick - client_timeout) {
					hlog(LOG_DEBUG, "%s: Closing client fd %d due to inactivity (%d s)",
						c->addr_rem, c->fd, client_timeout);
					client_close(self, c, CLIERR_INACTIVITY);
					continue;
				}
			}
		} else {
			if (c->last_read <= tick - upstream_timeout) {
				hlog(LOG_INFO, "%s: Closing uplink fd %d due to inactivity (%d s)",
				      c->addr_rem, c->fd, upstream_timeout);
				client_close(self, c, CLIERR_INACTIVITY);
				continue;
			}
		}
		
		/* check for write timeouts */
		if (c->obuf_wtime < w_expire && c->state != CSTATE_UDP) {
			// TOO OLD!  Shutdown the client
			hlog(LOG_DEBUG, "%s: Closing connection fd %d due to obuf wtime timeout",
			      c->addr_rem, c->fd);
			client_close(self, c, CLIERR_OUTPUT_WRITE_TIMEOUT);
			continue;
		}
		
		/* Adjust buffering, try not to jump back and forth between buffered and unbuffered.
		 * Please note that the we always flush the buffer at the end of a round if the
		 * client socket is writable (OS buffer not full), so we don't really wait for
		 * obuf_flushsize to be reached. Buffering will just make a couple of packets sent
		 * go in the same write().
		 */
		if (c->obuf_writes > obuf_writes_threshold) {
			// Lots and lots of writes, switch to buffering...
			if (c->obuf_flushsize == 0) {
				c->obuf_flushsize = c->obuf_size / 2;
				//hlog( LOG_DEBUG,"Switch fd %d (%s) to buffered writes (%d writes), flush at %d",
				//	c->fd, c->addr_rem, c->obuf_writes, c->obuf_flushsize);
			}
		} else if (c->obuf_flushsize != 0 && c->obuf_writes < obuf_writes_threshold_hys) {
			// Not so much writes, back to "write immediate"
			//hlog( LOG_DEBUG,"Switch fd %d (%s) to unbuffered writes (%d writes)",
			//	 c->fd, c->addr_rem, c->obuf_writes);
			c->obuf_flushsize = 0;
		}
		
		c->obuf_writes = 0;
	}
}


/*
 *	Worker thread
 */

void worker_thread(struct worker_t *self)
{
	sigset_t sigs_to_block;
	time_t next_keepalive = tick + 2;
	char myname[20];
	struct pbuf_t *p;
#if 0
	time_t next_lag_query = tick + 10;
#endif
	time_t t1, t2, t3, t4, t5, t6, t7;

	sprintf(myname,"worker %d", self->id);
	pthreads_profiling_reset(myname);

	sigemptyset(&sigs_to_block);
	sigaddset(&sigs_to_block, SIGALRM);
	sigaddset(&sigs_to_block, SIGINT);
	sigaddset(&sigs_to_block, SIGTERM);
	sigaddset(&sigs_to_block, SIGQUIT);
	sigaddset(&sigs_to_block, SIGHUP);
	sigaddset(&sigs_to_block, SIGURG);
	sigaddset(&sigs_to_block, SIGPIPE);
	sigaddset(&sigs_to_block, SIGUSR1);
	sigaddset(&sigs_to_block, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);
	
	hlog(LOG_DEBUG, "Worker %d started.", self->id);
	
	while (!self->shutting_down) {
		tick = now = time(NULL);
		t1 = tick;
		
		/* if we have new stuff in the global packet buffer, process it */
		if (*self->pbuf_global_prevp || *self->pbuf_global_dupe_prevp)
			process_outgoing(self);

		t2 = tick;

		// TODO: calculate different delay based on outgoing lag ?
		/* poll for incoming traffic */
		xpoll(&self->xp, 30); // was 200, but gave too big latency
		
		/* if we have stuff in the local queue, try to flush it and make
		 * it available to the dupecheck thread
		 */
		t3 = tick;

		if (self->pbuf_incoming_local)
			incoming_flush(self);
		
		t4 = tick;

		if (self->new_clients)
			collect_new_clients(self);

		t5 = tick;

		/* time of next keepalive broadcast ? */
		if (tick >= next_keepalive) {
			next_keepalive = tick + keepalive_poll_freq; /* Run them every 2 seconds */
			send_keepalives(self);
		}
		
		t6 = tick;
#if 0
		if (tick > next_lag_query) {
			int lag, lag1, lag2;
			next_lag_query += 10; // every 10 seconds..
			lag = outgoing_lag_report(self, &lag1, &lag2);
			hlog(LOG_DEBUG, "Thread %d  pbuf lag %d,  dupelag %d", self->id, lag1, lag2);
		}
#endif
		t7 = tick;

#if 1
		if (t7-t1 > 1 || t7-t1 < 0) // only report if the delay is over 1 seconds.  they are a LOT rarer
		  hlog( LOG_DEBUG, "Worker thread %d loop step delays:  dt2: %d  dt3: %d  dt4: %d  dt5: %d  dt6: %d  dt7: %d",
			self->id, t2-t1, t3-t1, t4-t1, t5-t1, t6-t1, t7-t1 );
#endif
	}
	
	if (self->shutting_down == 2) {
		/* live upgrade: must free all UDP client structs - we need to close the UDP listener fd. */
		struct client_t *c;
		for (c = self->clients; (c); c = c->next) {
			/* collect client state first before closing or freeing anything */
			if (worker_shutdown_clients) {
				cJSON *jc = worker_client_json(c, 1);
				cJSON_AddItemToArray(worker_shutdown_clients, jc);
			}
			client_udp_free(c->udpclient);
			c->udpclient = NULL;
		}
	} else {
		/* close all clients, if not shutting down for a live upgrade */
		while (self->clients)
			client_close(self, self->clients, CLIOK_THREAD_SHUTDOWN);
	}
	
	/* stop polling */
	xpoll_free(&self->xp);
	memset(&self->xp,0,sizeof(self->xp));
	
	/* check if there is stuff in the incoming queue (not taken by dupecheck) */
	int pbuf_incoming_found = 0;
	for (p = self->pbuf_incoming; p; p = p->next) {
		pbuf_incoming_found++;
	}
	if (pbuf_incoming_found != self->pbuf_incoming_count) {
		hlog(LOG_ERR, "Worker %d: found %d packets in incoming queue, does not match count %d",
			self->id, pbuf_incoming_found, self->pbuf_incoming_count);
	}
	if (self->pbuf_incoming_count)
		hlog(LOG_INFO, "Worker %d: %d packets left in incoming queue",
			self->id, self->pbuf_incoming_count);
	
	/* clean up thread-local pbuf pools */
	worker_free_buffers(self);
	
	hlog(LOG_DEBUG, "Worker %d shut down%s.", self->id, (self->shutting_down == 2) ? " - clients left hanging" : "");
}

/*
 *	Stop workers - runs from accept_thread
 *	stop_all: 1 => stop all threads, 2 => stop all threads for live upgrade
 */

void workers_stop(int stop_all)
{
	struct worker_t *w;
	int e;
	int stopped = 0;
	
	hlog(LOG_INFO, "Stopping %d worker threads...",
		(stop_all) ? workers_running : workers_running - workers_configured);
	while (workers_running > workers_configured || (stop_all && workers_running > 0)) {
		hlog(LOG_DEBUG, "Stopping a worker thread...");
		/* find the last worker thread and shut it down...
		 * could shut down the first one, but to reduce confusion
		 * will shut down the one with the largest worker id :)
		 *
		 * This could be done even more cleanly by moving the connected
		 * clients to the threads which are left running, but maybe
		 * that'd be way too cool and complicated to implement right now.
		 * It's cool enough to be able to reconfigure at all.
		 */
		w = worker_threads;
		while ((w) && (w->next))
			w = w->next;
		
		w->shutting_down = (stop_all == 2) ? 2 : 1;
		if ((e = pthread_join(w->th, NULL))) {
			hlog(LOG_ERR, "Could not pthread_join worker %d: %s", w->id, strerror(e));
		} else {
			hlog(LOG_DEBUG, "Worker %d has terminated.", w->id);
			stopped++;
		}

		*(w->prevp) = NULL;
		hfree(w);
		
		workers_running--;
	}
	hlog(LOG_INFO, "Stopped %d worker threads.", stopped);
	
}

/*
 *	Allocate a worker structure.
 *	This is also called from the http thread which acts as a
 *	"worker" for incoming packets.
 */

struct worker_t *worker_alloc(void)
{
	struct worker_t *w;
	pthread_mutexattr_t mut_recursive;
	int e;
	
	if ((e = pthread_mutexattr_init(&mut_recursive))) {
		hlog(LOG_ERR, "worker_alloc: pthread_mutexattr_init failed: %s", strerror(e));
	}
	
	if ((e = pthread_mutexattr_settype(&mut_recursive, PTHREAD_MUTEX_RECURSIVE))) {
		hlog(LOG_ERR, "worker_alloc: pthread_mutexattr_settype PTHREAD_MUTEX_RECURSIVE failed: %s", strerror(e));
	}
	
	w = hmalloc(sizeof(*w));
	memset(w, 0, sizeof(*w));

	pthread_mutex_init(&w->clients_mutex, &mut_recursive);
	pthread_mutex_init(&w->new_clients_mutex, NULL);
	
	w->pbuf_incoming_local = NULL;
	w->pbuf_incoming_local_last = &w->pbuf_incoming_local;
	
	w->pbuf_incoming      = NULL;
	w->pbuf_incoming_last = &w->pbuf_incoming;
	pthread_mutex_init(&w->pbuf_incoming_mutex, NULL);
	
	w->pbuf_global_prevp      = pbuf_global_prevp;
	w->pbuf_global_dupe_prevp = pbuf_global_dupe_prevp;
	
	return w;
}

/*
 *	Free a worker's local buffers
 */

void worker_free_buffers(struct worker_t *self)
{
	struct pbuf_t *p, *pn;
	
	/* clean up thread-local pbuf pools */
	for (p = self->pbuf_free_small; p; p = pn) {
		pn = p->next;
		pbuf_free(NULL, p); // free to global pool
	}
	for (p = self->pbuf_free_medium; p; p = pn) {
		pn = p->next;
		pbuf_free(NULL, p); // free to global pool
	}
	for (p = self->pbuf_free_large; p; p = pn) {
		pn = p->next;
		pbuf_free(NULL, p); // free to global pool
	}
}

/*
 *	Start workers - runs from accept_thread
 */

void workers_start(void)
{
	int i;
	struct worker_t * volatile w;
 	struct worker_t **prevp;
	
	if (workers_running)
		workers_stop(0);
	
	hlog(LOG_INFO, "Starting %d worker threads (configured: %d)...",
		workers_configured - workers_running, workers_configured);
		
	while (workers_running < workers_configured) {
		hlog(LOG_DEBUG, "Starting a worker thread...");
		i = 0;
		prevp = &worker_threads;
		w = worker_threads;
		while (w) {
			prevp = &w->next;
			w = w->next;
			i++;
		}
		
		w = worker_alloc();
		*prevp = w;
		w->prevp = prevp;
		
		w->id = i;
		xpoll_initialize(&w->xp, (void *)w, &handle_client_event);
		
		/* start the worker thread */
		if (pthread_create(&w->th, &pthr_attrs, (void *)worker_thread, w))
			perror("pthread_create failed for worker_thread");
		
		workers_running++;
	}
}

/*
 *	Add an array of long longs to a JSON tree.
 */

void json_add_rxerrs(cJSON *root, const char *key, long long vals[])
{
	double vald[INERR_BUCKETS];
	int i;
	
	/* cJSON does not have a CreateLongLongArray, big ints are taken in
	 * as floating point values. Strange, ain't it.
	 */
	for (i = 0; i < INERR_BUCKETS; i++)
		vald[i] = vals[i];
	
	cJSON_AddItemToObject(root, key, cJSON_CreateDoubleArray(vald, INERR_BUCKETS));
}

/*
 *	Client state string
 */

static const char *client_state_string(CStateEnum state)
{
	static const char *states[] = {
		"unknown",
		"init",
		"login",
		"logresp",
		"connected",
		"udp",
		"corepeer"
	};
	
	switch (state) {
	case CSTATE_CONNECTED:
		return states[4];
	case CSTATE_INIT:
		return states[1];
	case CSTATE_LOGIN:
		return states[2];
	case CSTATE_LOGRESP:
		return states[3];
	case CSTATE_UDP:
		return states[5];
	case CSTATE_COREPEER:
		return states[6];
	};
	
	return states[0];
}

/*
 *	Fill worker client list for status display
 *	(called from another thread - watch out and lock!)
 */

static struct cJSON *worker_client_json(struct client_t *c, int liveup_info)
{
	char addr_s[80];
	char *s;
	static const char *uplink_modes[] = {
		"ro",
		"multiro",
		"full",
		"peer"
	};
	const char *mode;
	
	cJSON *jc = cJSON_CreateObject();
	cJSON_AddNumberToObject(jc, "fd", c->fd);
	
	/* additional information for live upgrade, not published */
	if (liveup_info) {
		cJSON_AddNumberToObject(jc, "listener_id", c->listener_id);
		cJSON_AddStringToObject(jc, "state", client_state_string(c->state));
		if (c->udp_port && c->udpclient)
			cJSON_AddNumberToObject(jc, "udp_port", c->udp_port);
		
		/* output buffer and input buffer data */
		if (c->obuf_end - c->obuf_start > 0) {
			s = hex_encode(c->obuf + c->obuf_start, c->obuf_end - c->obuf_start);
			cJSON_AddStringToObject(jc, "obuf", s);
			hfree(s);
		}
		
		if (c->ibuf_end > 0) {
			s = hex_encode(c->ibuf, c->ibuf_end);
			cJSON_AddStringToObject(jc, "ibuf", s);
			hlog(LOG_DEBUG, "Encoded ibuf %d bytes: '%.*s'", c->ibuf_end, c->ibuf_end, c->ibuf);
			hlog(LOG_DEBUG, "Hex: %s", s);
			hfree(s);
		}
		
		/* If message routing for stations heard by this client is enabled,
		 * dump the client_heard hash table, too.
		 */
		if (c->flags & CLFLAGS_IGATE)
			cJSON_AddItemToObject(jc, "client_heard", client_heard_json(c->client_heard));
	}
	
	if (c->state == CSTATE_COREPEER) {
		/* cut out ports in the name of security by obscurity */
		strncpy(addr_s, c->addr_rem, sizeof(addr_s));
		if ((s = strrchr(addr_s, ':')))
			*s = 0;
		cJSON_AddStringToObject(jc, "addr_rem", addr_s);
		strncpy(addr_s, c->addr_loc, sizeof(addr_s));
		if ((s = strrchr(addr_s, ':')))
			*s = 0;
		cJSON_AddStringToObject(jc, "addr_loc", addr_s);
	} else {
		cJSON_AddStringToObject(jc, "addr_rem", c->addr_rem);
		cJSON_AddStringToObject(jc, "addr_loc", c->addr_loc);
	}
	
	//cJSON_AddStringToObject(jc, "addr_q", c->addr_hex);
	
	if (c->udp_port && c->udpclient)
		cJSON_AddNumberToObject(jc, "udp_downstream", 1);
	
	cJSON_AddNumberToObject(jc, "t_connect", c->connect_time);
	cJSON_AddNumberToObject(jc, "since_connect", tick - c->connect_time);
	cJSON_AddNumberToObject(jc, "since_last_read", tick - c->last_read);
	cJSON_AddStringToObject(jc, "username", c->username);
	cJSON_AddStringToObject(jc, "app_name", c->app_name);
	cJSON_AddStringToObject(jc, "app_version", c->app_version);
	cJSON_AddNumberToObject(jc, "verified", c->validated);
	cJSON_AddNumberToObject(jc, "obuf_q", c->obuf_end - c->obuf_start);
	cJSON_AddNumberToObject(jc, "bytes_rx", c->localaccount.rxbytes);
	cJSON_AddNumberToObject(jc, "bytes_tx", c->localaccount.txbytes);
	cJSON_AddNumberToObject(jc, "pkts_rx", c->localaccount.rxpackets);
	cJSON_AddNumberToObject(jc, "pkts_tx", c->localaccount.txpackets);
	cJSON_AddNumberToObject(jc, "pkts_ign", c->localaccount.rxdrops);
	cJSON_AddNumberToObject(jc, "pkts_dup", c->localaccount.rxdupes);
	cJSON_AddNumberToObject(jc, "heard_count", c->client_heard_count);
	cJSON_AddNumberToObject(jc, "courtesy_count", c->client_courtesy_count);
	
	if (c->quirks_mode)
		cJSON_AddNumberToObject(jc, "quirks_mode", c->quirks_mode);
	
	json_add_rxerrs(jc, "rx_errs", c->localaccount.rxerrs);
	
	if (c->state == CSTATE_COREPEER) {
		cJSON_AddStringToObject(jc, "mode", uplink_modes[3]);
	} else if (c->flags & CLFLAGS_INPORT) {
		/* client */
		cJSON_AddStringToObject(jc, "filter", c->filter_s);
	} else {
		if (c->flags & CLFLAGS_UPLINKMULTI)
			mode = uplink_modes[1];
		else if (c->flags & CLFLAGS_PORT_RO)
			mode = uplink_modes[0];
		else
			mode = uplink_modes[2];
			
		cJSON_AddStringToObject(jc, "mode", mode);
	}
	
	return jc;
}

int worker_client_list(cJSON *workers, cJSON *clients, cJSON *uplinks, cJSON *peers, cJSON *totals, cJSON *memory)
{
	struct worker_t *w = worker_threads;
	struct client_t *c;
	int pe;
	int client_heard_count = 0;
	int client_courtesy_count = 0;
	
	while (w) {
		if ((pe = pthread_mutex_lock(&w->clients_mutex))) {
			hlog(LOG_ERR, "worker_client_list(worker %d): could not lock clients_mutex: %s", w->id, strerror(pe));
			return -1;
		}
		
		cJSON *jw = cJSON_CreateObject();
		cJSON_AddNumberToObject(jw, "id", w->id);
		cJSON_AddNumberToObject(jw, "clients", w->client_count);
		cJSON_AddNumberToObject(jw, "pbuf_incoming_count", w->pbuf_incoming_count);
		cJSON_AddNumberToObject(jw, "pbuf_incoming_local_count", w->pbuf_incoming_local_count);
		
		for (c = w->clients; (c); c = c->next) {
			client_heard_count += c->client_heard_count;
			client_courtesy_count += c->client_courtesy_count;
			
			/* clients on hidden listener sockets are not shown */
			/* if there are a huge amount of clients, don't list them
			 * - cJSON takes huge amounts of CPU to build the list
			 * - web browser will die due to the big blob
			 */
			if (c->hidden || w->client_count > 1000)
				continue;
				
			cJSON *jc = worker_client_json(c, 0);
			
			if (c->state == CSTATE_COREPEER) {
				cJSON_AddItemToArray(peers, jc);
			} else if (c->flags & CLFLAGS_INPORT) {
				cJSON_AddItemToArray(clients, jc);
			} else {
				cJSON_AddItemToArray(uplinks, jc);
			}
		}
		
		cJSON_AddItemToArray(workers, jw);
		
		if ((pe = pthread_mutex_unlock(&w->clients_mutex))) {
			hlog(LOG_ERR, "worker_client_list(worker %d): could not unlock clients_mutex: %s", w->id, strerror(pe));
			/* we'd going to deadlock here... */
			exit(1);
		}
		
		w = w->next;
	}
	
	cJSON_AddNumberToObject(totals, "tcp_bytes_rx", client_connects_tcp.rxbytes);
	cJSON_AddNumberToObject(totals, "tcp_bytes_tx", client_connects_tcp.txbytes);
	cJSON_AddNumberToObject(totals, "udp_bytes_rx", client_connects_udp.rxbytes);
	cJSON_AddNumberToObject(totals, "udp_bytes_tx", client_connects_udp.txbytes);
	cJSON_AddNumberToObject(totals, "tcp_pkts_rx", client_connects_tcp.rxpackets);
	cJSON_AddNumberToObject(totals, "tcp_pkts_tx", client_connects_tcp.txpackets);
	cJSON_AddNumberToObject(totals, "udp_pkts_rx", client_connects_udp.rxpackets);
	cJSON_AddNumberToObject(totals, "udp_pkts_tx", client_connects_udp.txpackets);
	cJSON_AddNumberToObject(totals, "tcp_pkts_ign", client_connects_tcp.rxdrops);
	cJSON_AddNumberToObject(totals, "udp_pkts_ign", client_connects_udp.rxdrops);
	json_add_rxerrs(totals, "tcp_rx_errs", client_connects_tcp.rxerrs);
	json_add_rxerrs(totals, "udp_rx_errs", client_connects_udp.rxerrs);

#ifndef _FOR_VALGRIND_
	struct cellstatus_t cellst;
	cellstatus(client_cells, &cellst);
	int used = cellst.cellcount - cellst.freecount;
	cJSON_AddNumberToObject(memory, "client_cells_used", used);
	cJSON_AddNumberToObject(memory, "client_cells_free", cellst.freecount);
	cJSON_AddNumberToObject(memory, "client_used_bytes", used*cellst.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "client_allocated_bytes", (long)cellst.blocks * (long)cellst.block_size);
	cJSON_AddNumberToObject(memory, "client_block_size", (long)cellst.block_size);
	cJSON_AddNumberToObject(memory, "client_blocks", (long)cellst.blocks);
	cJSON_AddNumberToObject(memory, "client_blocks_max", (long)cellst.blocks_max);
	cJSON_AddNumberToObject(memory, "client_cell_size", cellst.cellsize);
	cJSON_AddNumberToObject(memory, "client_cell_size_aligned", cellst.cellsize_aligned);
	cJSON_AddNumberToObject(memory, "client_cell_align", cellst.alignment);
#endif
	
	return 0;
}
