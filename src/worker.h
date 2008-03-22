/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
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

#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>

#include "xpoll.h"
#include "rwlock.h"

extern time_t now;	/* current time - updated by the main thread, MAY be running under simulator */
extern time_t tick;	/* clocktick - monotonously increasing, never in simulator */

extern void pthreads_profiling_reset(const char *name);

/* minimum and maximum length of a callsign on APRS-IS */
#define CALLSIGNLEN_MIN 3
#define CALLSIGNLEN_MAX 9

/* packet length limiters and buffer sizes */
#define PACKETLEN_MIN 10	/* minimum length for a valid APRS-IS packet: "A1A>B1B:\r\n" */
#define PACKETLEN_MAX 512	/* maximum length for a valid APRS-IS packet (incl. CRLF) */

#define PACKETLEN_MAX_SMALL 130
#define PACKETLEN_MAX_LARGE 300
#define PACKETLEN_MAX_HUGE PACKETLEN_MAX

/* number of pbuf_t structures to allocate at a time */
#define PBUF_ALLOCATE_BUNCH_SMALL 2000 /* grow to 2000 in production use - it's now small for debugging */
#define PBUF_ALLOCATE_BUNCH_LARGE 2000 /* grow to 2000 in production use - it's now small for debugging */
#define PBUF_ALLOCATE_BUNCH_HUGE    50 /* grow to 50 in production use - it's now small for debugging */

/* a packet buffer */
/* Type flags -- some can happen in combinations: T_CWOP + T_WX / T_CWOP + T_POSITION ... */
#define T_POSITION  (1 << 0) // Packet is of position type
#define T_OBJECT    (1 << 1) // packet is an object
#define T_ITEM      (1 << 2) // packet is an item
#define T_MESSAGE   (1 << 3) // packet is a message
#define T_NWS       (1 << 4) // packet is a NWS message
#define T_WX        (1 << 5) // packet is WX data
#define T_TELEMETRY (1 << 6) // packet is telemetry
#define T_QUERY     (1 << 7) // packet is a query
#define T_STATUS    (1 << 8) // packet is status 
#define T_USERDEF   (1 << 9) // packet is userdefined
#define T_CWOP      (1 << 10) // packet is recognized as CWOP

#define F_DUPE    1	/* Duplicate of a previously seen packet */
#define F_HASPOS  2	/* This packet has valid parsed position */

struct client_t; /* forward declarator */

struct pbuf_t {
	struct pbuf_t *next;
	struct client_t *origin;
		/* where did we get it from (don't send it back)
		   NOTE: This pointer is NOT guaranteed to be valid!
		   It does get invalidated by originating connection close,
		   but it is only used as read-only comparison reference
		   against output client connection pointer.  It may
		   point to reused connection entry, but even that does
		   not matter much -- a few packets may be left unrelayed
		   to the a client in some situations, but packets sent
		   a few minutes latter will go through just fine. 
		   In case of "dump history" (if we ever do) this pointer
		   is ignored while history dumping is being done.
		*/

	time_t t;		/* when the packet was received */
	uint32_t seqnum;	/* ever increasing counter, dupecheck sets */
	uint16_t packettype;	/* bitmask: one or more of T_* */
	uint16_t flags;		/* bitmask: one or more of F_* */
	uint16_t srcname_len;	/* parsed length of source (object, item, srcall) name 3..9 */
	uint16_t dstcall_len;	/* parsed length of destination callsign *including* SSID */
	uint16_t entrycall_len;
	
	int packet_len;		/* the actual length of the packet, including CRLF */
	int buf_len;		/* the length of this buffer */
	
	const char *srccall_end;   /* source callsign with SSID */
	const char *dstcall_end;   /* end of dest callsign SSID */
	const char *qconst_start;  /* "qAX,incomingSSID:"	-- for q and e filters  */
	const char *info_start;    /* pointer to start of info field */
	const char *srcname;       /* source's name (either srccall or object/item name) */
	
	float lat;	/* if the packet is PT_POSITION, latitude and longitude go here */
	float lng;	/* .. in RADIAN */
	float cos_lat;	/* cache of COS of LATitude for radial distance filter    */

	char symbol[3]; /* 2(+1) chars of symbol, if any, NUL for not found */

	char data[1];	/* contains the whole packet, including CRLF, ready to transmit */
};

/* global packet buffer */
extern rwlock_t pbuf_global_rwlock;
extern struct pbuf_t  *pbuf_global;
extern struct pbuf_t  *pbuf_global_last;
extern struct pbuf_t **pbuf_global_prevp;
extern struct pbuf_t  *pbuf_global_dupe;
extern struct pbuf_t  *pbuf_global_dupe_last;
extern struct pbuf_t **pbuf_global_dupe_prevp;

/* a network client */
#define CSTATE_LOGIN      0
#define CSTATE_CONNECTED  1
#define CSTATE_COREPEER   2
#define CSTATE_UPLINK     3
#define CSTATE_UPLINKSIM  4

struct worker_t; /* used in client_t, but introduced later */
struct filter_t; /* used in client_t, but introduced later */

union sockaddr_u {
	struct sockaddr     sa;
	struct sockaddr_in  si;
	struct sockaddr_in6 si6;
};

#define WBUF_ADJUSTER 0   /* Client WBUF adjustment can be usefull -- but code is infant.. */


struct client_t {
	struct client_t *next;
	struct client_t **prevp;
	
	union sockaddr_u addr;
	int    fd;
	int    udp_port;
	char  *addr_s;	    /* client IP address in text format */
	int    portnum;
	time_t keepalive;   /* Time of next keepalive chime */
	time_t logintimeout; /* when the login wait times out */

	struct xpoll_fd_t *xfd; /* poll()/select() structure as defined in xpoll.h */
	
	/* first stage read buffer - used to crunch out lines to packet buffers */
	char *ibuf;
	int   ibuf_size;      /* size of buffer */
	int   ibuf_end;       /* where data in buffer ends */
	
	/* output buffer */
	char *obuf;
	int   obuf_size;      /* size of buffer */
	int   obuf_start;     /* where data in buffer starts */
	int   obuf_end;       /* where data in buffer ends */
	int   obuf_flushsize; /* how much data in buf before forced write() at adding ? */
	int   obuf_writes;    /* how many times (since last check) the socket has been written ? */
	int   obuf_wtime;     /* when was last write? */
#if WBUF_ADJUSTER
	int   wbuf_size;      /* socket wbuf size */
#endif
	int32_t flags;      /* bit flags on what kind of client this is */

#define CLFLAGS_USERFILTEROK   0x001 /* Permits entry of user defined filters */
#define CLFLAGS_UPLINKPORT     0x002
#define CLFLAGS_PORT_RO	       0x004
#define CLFLAGS_FULLFEED       0x008 /* Together with filter t/.*. -- which really implements it */
#define CLFLAGS_DUPEFEED       0x008 /* Duplicates are also sent to client */
#define CLFLAGS_MESSAGEONLY    0x010 /* Together with filter t/m   -- which really implements it */

	char  state;        /* state of the client... one of CSTATE_* */
	char  warned;       /* the client has been warned that it has bad filter definition */
	char *username;     /* The callsign */
	char *app_name;     /* application name, from 'user' command */
	char *app_version;  /* application version, from 'user' command */

	char  validated;    /* did the client provide a valid passcode */
	
	/* the current handler function for incoming lines */
	int	(*handler)	(struct worker_t *self, struct client_t *c, char *s, int len);

	/* outbound filter chain head */
	struct filter_t *defaultfilters;
	struct filter_t *userfilters;


	// Maybe we use these four items, or maybe not.
	// They are there for experimenting with outgoing queue processing algorithms.

	/* Pointer to last pointer in pbuf_global(_dupe) */
	struct pbuf_t **pbuf_global_prevp;
	struct pbuf_t **pbuf_global_dupe_prevp;

	uint32_t	last_pbuf_seqnum;
	uint32_t	last_pbuf_dupe_seqnum;
};

extern struct client_t *client_alloc(void);
extern void client_free(struct client_t *c);


/* worker thread structure */
struct worker_t {
	struct worker_t *next;
	struct worker_t **prevp;
	
	int id;			/* sequential id for thread */
	pthread_t th;		/* the thread itself */
	
	int shutting_down;			/* should I shut down? */
	
	struct client_t *clients;		/* clients handled by this thread */
	
	struct client_t *new_clients;		/* new clients which passed in by accept */
	pthread_mutex_t new_clients_mutex;	/* mutex to protect *new_clients */
	int client_count;			/* modified by worker thread only! */
	
	struct xpoll_t *xp;			/* poll/epoll/select wrapper */
	
	/* thread-local packet buffer freelist */
	struct pbuf_t *pbuf_free_small; /* <= 130 bytes */
	struct pbuf_t *pbuf_free_large; /* 131 >= x <= 300 */
	struct pbuf_t *pbuf_free_huge; /* 301 >= x <= 600 */
	
	/* packets which have been parsed, waiting to be moved into
	 * pbuf_incoming
	 */
  int pbuf_incoming_local_count; // debug stuff
  int pbuf_incoming_count;       // debug stuff

	struct pbuf_t *pbuf_incoming_local;
	struct pbuf_t **pbuf_incoming_local_last;
	
	/* packets which have been parsed, waiting for dupe check */
	struct pbuf_t *pbuf_incoming;
	struct pbuf_t **pbuf_incoming_last;
	pthread_mutex_t pbuf_incoming_mutex;
	
	/* Pointer to last pointer in pbuf_global(_dupe) */
	struct pbuf_t **pbuf_global_prevp;
	struct pbuf_t **pbuf_global_dupe_prevp;

	uint32_t	last_pbuf_seqnum;
	uint32_t	last_pbuf_dupe_seqnum;
};

extern void pbuf_init(void);
extern void pbuf_free(struct worker_t *self, struct pbuf_t *p);
extern void pbuf_free_many(struct pbuf_t **array, int numbufs);

extern int client_printf(struct worker_t *self, struct client_t *c, const char *fmt, ...);
extern int client_write(struct worker_t *self, struct client_t *c, char *p, int len);
extern int client_bad_filter_notify(struct worker_t *self, struct client_t *c, const char *filt);

extern struct worker_t *worker_threads;
extern void workers_stop(int stop_all);
extern void workers_start(void);

extern int keepalive_interval;
extern int fileno_limit;

#endif
