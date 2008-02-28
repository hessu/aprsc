
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
#define T_POSITION 1
#define T_MESSAGE 2
#define T_WX 4
#define T_BULLETIN 8
#define T_TELEMETRY 16

#define F_DUPE 1	/* duplicate of a previously seen packet */

struct client_t; /* forward declarator */

struct pbuf_t {
	struct pbuf_t *next;
	struct client_t *origin; /* where did we get it from (don't send it back)
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
	char *data;		/* contains the whole packet, including CRLF, ready to transmit */
	
	char *srccall_end;	/* source callsign with SSID */
	char *dstcall_end;	/* end of dest callsign SSID */
	char *info_start;	/* pointer to start of info field */
	
	float lat; /* if the packet is PT_POSITION, latitude and longitude go here */
	float lng;
	
	struct sockaddr addr;	/* where did we get it from (don't send it back) */
};

/* global packet buffer freelists */
extern pthread_mutex_t pbuf_free_small_mutex;
extern struct pbuf_t *pbuf_free_small;
extern pthread_mutex_t pbuf_free_large_mutex;
extern struct pbuf_t *pbuf_free_large;
extern pthread_mutex_t pbuf_free_huge_mutex;
extern struct pbuf_t *pbuf_free_huge;

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


struct client_t {
	struct client_t *next;
	struct client_t **prevp;
	
	struct sockaddr addr;
	int fd;
	int udp_port;
	char *addr_s;
	
	struct xpoll_fd_t *xfd;
	
	/* first stage read buffer - used to crunch out lines to packet buffers */
	char *ibuf;
	int ibuf_size; /* size of buffer */
	int ibuf_end; /* where data in buffer ends */
	
	/* output buffer */
	char *obuf;
	int obuf_size; /* size of buffer */
	int obuf_start; /* where data in buffer starts */
	int obuf_end; /* where data in buffer ends */
	
	/* state of the client... one of CSTATE_* */
	int state;
	char *username;
	char *app_name;
	char *app_version;
	int validated;		/* did the client provide a valid passcode */
	
	/* the current handler function for incoming lines */
	int	(*handler)	(struct worker_t *self, struct client_t *c, char *s, int len);

	/* outbound filter chain head */
	struct filter_t *filterhead;
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

extern int client_printf(struct worker_t *self, struct client_t *c, const char *fmt, ...);
extern int client_write(struct worker_t *self, struct client_t *c, char *p, int len);

extern struct worker_t *worker_threads;
extern void workers_stop(int stop_all);
extern void workers_start(void);

#endif
