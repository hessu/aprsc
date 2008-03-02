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

extern time_t now;	/* current time - updated by the main thread */

#define CALLSIGNLEN_MAX 9

/* packet length limiters and buffer sizes */
#define PACKETLEN_MIN 4		/* minimum length for a valid APRS-IS packet: "A>B:" */
#define PACKETLEN_MAX 600	/* max... arbitrary and not documented */

#define PACKETLEN_MAX_SMALL 130
#define PACKETLEN_MAX_LARGE 300
#define PACKETLEN_MAX_HUGE PACKETLEN_MAX

/* number of pbuf_t structures to allocate at a time */
#define PBUF_ALLOCATE_BUNCH_SMALL 20 /* grow to 2000 in production use - it's now small for debugging */
#define PBUF_ALLOCATE_BUNCH_LARGE 20 /* grow to 2000 in production use - it's now small for debugging */
#define PBUF_ALLOCATE_BUNCH_HUGE 5 /* grow to 50 in production use - it's now small for debugging */

/* a packet buffer */
#define T_POSITION  (1 << 0) /* Every time the packet coordinates are determined, this is also set! */
#define T_OBJECT    (1 << 1)
#define T_ITEM      (1 << 2)
#define T_MESSAGE   (1 << 3)
#define T_NWS       (1 << 4)
#define T_WX        (1 << 5)
#define T_TELEMETRY (1 << 6)
#define T_QUERY     (1 << 7)
#define T_STATUS    (1 << 8)
#define T_USERDEF   (1 << 9)
#define T_CWOP      (1 << 10)

#define F_DUPE    1	/* Duplicate of a previously seen packet */
#define F_DUPEKEY 2	/* First of the unique keys..
			   Tells also that it is in dupe database search tree */
#define F_LASTPOS 4	/* Last position packet of given object/source id.
			   Tells also that it is in history DB position keys */

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
	int packettype;		/* bitmask: one or more of T_* */
	int flags;		/* bitmask: one or more of F_* */
	
	int packet_len;		/* the actual length of the packet, including CRLF */
	int buf_len;		/* the length of this buffer */
	
	char *srccall_end;	/* source callsign with SSID */
	char *dstcall_end;	/* end of dest callsign SSID */
	char *info_start;	/* pointer to start of info field */
	
	float lat;	/* if the packet is PT_POSITION, latitude and longitude go here */
	float lng;	/* .. in RADIAN */
	float cos_lat;	/* cache of COS of LATitude for radial distance filter    */

	char symbol[4]; /* 3(+1) chars of symbol, if any */

	char data[1];	/* contains the whole packet, including CRLF, ready to transmit */
};

/* global packet buffer */
extern rwlock_t pbuf_global_rwlock;
extern struct pbuf_t *pbuf_global;
extern struct pbuf_t *pbuf_global_last;
extern struct pbuf_t **pbuf_global_prevp;

/* a network client */
#define CSTATE_LOGIN 0
#define CSTATE_CONNECTED 1

struct worker_t; /* used in client_t, but introduced later */
struct filter_t; /* used in client_t, but introduced later */

union sockaddr_u {
	struct sockaddr     sa;
	struct sockaddr_in  si;
	struct sockaddr_in6 si6;
};



struct client_t {
	struct client_t *next;
	struct client_t **prevp;
	
	union sockaddr_u addr;
	int fd;
	int udp_port;
	char *addr_s;	    /* client IP address in text format */
	time_t keepalive;   /* Time of next keepalive chime */

	struct xpoll_fd_t *xfd;
	
	/* first stage read buffer - used to crunch out lines to packet buffers */
	char *ibuf;
	int ibuf_size;      /* size of buffer */
	int ibuf_end;       /* where data in buffer ends */
	
	/* output buffer */
	char *obuf;
	int obuf_size;      /* size of buffer */
	int obuf_start;     /* where data in buffer starts */
	int obuf_end;       /* where data in buffer ends */
	int obuf_flushsize; /* how much data in buf before forced write() at adding ? */
	int obuf_writes;    /* how many times (since last check) the socket has been written ? */

	/* state of the client... one of CSTATE_* */
	char state;
	char warned;
	char *username;     /* The callsign */
	char *app_name;
	char *app_version;
	int validated;	    /* did the client provide a valid passcode */
	
	/* the current handler function for incoming lines */
	int	(*handler)	(struct worker_t *self, struct client_t *c, char *s, int len);

	/* outbound filter chain head */
	struct filter_t *filterhead;
	float my_lat, my_lon, my_coslat; /* Cache of my last known coordinates */

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
	struct pbuf_t *pbuf_incoming_local;
	struct pbuf_t **pbuf_incoming_local_last;
	
	/* packets which have been parsed, waiting for dupe check */
	struct pbuf_t *pbuf_incoming;
	struct pbuf_t **pbuf_incoming_last;
	pthread_mutex_t pbuf_incoming_mutex;
	
	/* Pointer to last pointer in pbuf_global */
	struct pbuf_t **pbuf_global_prevp;
};

extern void pbuf_init(void);
extern void pbuf_free(struct worker_t *self, struct pbuf_t *p);
extern void pbuf_free_many(struct pbuf_t **array, int numbufs);

extern int client_printf(struct worker_t *self, struct client_t *c, const char *fmt, ...);
extern int client_write(struct worker_t *self, struct client_t *c, char *p, int len);
extern void client_bad_filter_notify(struct worker_t *self, struct client_t *c, const char *filt);

extern struct worker_t *worker_threads;
extern void workers_stop(int stop_all);
extern void workers_start(void);

extern int keepalive_interval;

#endif
