/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#include "ac-hdrs.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

#include "hmalloc.h"
#include "hlog.h"
#include "worker.h"
#include "filter.h"
#include "cellmalloc.h"
#include "historydb.h"
#include "cfgfile.h"
#include "config.h"
#include "keyhash.h"
#include "client_heard.h"
#include "version.h"
#include "messaging.h"

//#define FILTER_CLIENT_DEBUGGING

#ifdef FILTER_CLIENT_DEBUGGING
#define FILTER_CLIENT_DEBUG(worker, c, fmt, ...) \
            do { client_printf(worker, c, fmt, __VA_ARGS__); } while (0)
#else
#define FILTER_CLIENT_DEBUG(fmt, ...)
#endif


// static double rad2deg(double a) { return a * (180.0 * M_1_PI); }

/*
  See:  http://www.aprs-is.net/javaprssrvr/javaprsfilter.htm

  a/latN/lonW/latS/lonE Area filter
  b/call1/call2...  	Budlist filter (*)
  d/digi1/digi2...  	USED Digipeater filter (*)
  e/call1/call1/...  	Entry station filter (*)
  f/call/dist  		Friend Range filter
  m/dist  		My Range filter
  o/obj1/obj2...  	Object filter (*)
  p/aa/bb/cc...  	Prefix filter
  q/con/ana 	 	q Contruct filter
  r/lat/lon/dist  	Range filter
  s/pri/alt/over  	Symbol filter
  t/poimntqsu*c		Type filter
  t/poimntqsu*c/call/km	Type filter
  u/unproto1/unproto2/.. Unproto filter (*)
  g/call/call2/...      Message destination filter (*)

  Sample usage frequencies (out of entire APRS-IS):

   23.7  a/  <-- Optimize!
    9.2  b/  <-- Optimize?
    1.4  d/
    0.2  e/
    2.2  f/
   20.9  m/  <-- Optimize!
    0.2  o/
   14.4  p/  <-- Optimize!
    0.0  pk
    0.0  pm
    0.4  q/
   19.0  r/  <-- Optimize!
    0.1  s_
    1.6  s/
    6.6  t/
    0.1  u/


  (*) = wild-card supported

  Undocumented at above web-page, but apparent behaviour is:

  - Everything not explicitely stated to be case sensitive is
    case INSENSITIVE

  - Minus-prefixes on filters behave as is there are two sets of
    filters:

       - filters without minus-prefixes add on approved set, and all
         those without are evaluated at first
       - filters with minus-prefixes are evaluated afterwards to drop
         selections after the additive filter has been evaluated


  - Our current behaviour is: "evaluate everything in entry order,
    stop at first match",  which enables filters like:
               p/OH2R -p/OH2 p/OH
    while javAPRSSrvr filter adjunct behaves like the request is:
               -p/OH2  p/OH
    that is, OH2R** stations are not passed thru.

*/


/* FIXME:  What exactly is the meaning of negation on the pattern ?
**         Match as a failure to match, and stop searching ?
**         Something filter dependent ?
**
** javAPRSSrvr Filter Adjunct  manual tells:

 #14 Exclusion filter

All the above filters also support exclusion. Be prefixing the above filters
with a dash the result will be the opposite. Any packet that match the exclusion
filter will NOT pass. The exclusion filters will be processed first so if there
is a match for an exclusion then the packet is not passed no matter any other
filter definitions.

*/

#define WildCard      0x80  /* it is wild-carded prefix string  */
#define NegationFlag  0x40  /*                                  */
#define LengthMask    0x0F  /* only low 4 bits encode length    */

/* values above are chosen for 4 byte alignment.. */

struct filter_refcallsign_t {
	char	callsign[CALLSIGNLEN_MAX+1]; /* size: 10.. */
	int8_t	reflen; /* length and flags */
};
struct filter_head_t {
	struct filter_t *next;
	const char *text; /* filter text as is		*/
	float   f_latN, f_lonE;
	union {
	  float   f_latS;   /* for A filter */
	  float   f_coslat; /* for R filter */
	}; /* ANONYMOUS UNION */
	union {
	  float   f_lonW; /* for A filter */
	  float   f_dist; /* for R filter */
	}; /* ANONYMOUS UNION */
	time_t  hist_age;

	char	type;	  /* 1 char			*/
	int16_t	negation; /* boolean flag		*/
	union {
	  int16_t numnames; /* used as named, and as cache validity flag */
	  int16_t len1s;    /*  or len1 of s-filter */
	}; /* ANONYMOUS UNION */
	union {
	  int16_t bitflags; /* used as bit-set on T_*** enumerations */
	  int16_t len1;     /*  or as len2 of s-filter */
	}; /* ANONYMOUS UNION */
	union {
		struct lens_t { int16_t len2s, len2, len3s, len3; } lens; /* of s-filter */
		/* for cases where there is only one.. */
		struct filter_refcallsign_t  refcallsign;
		/*  hmalloc()ed array, alignment important! */
		struct filter_refcallsign_t *refcallsigns;
	}; /* ANONYMOUS UNION */
};

struct filter_t {
	struct filter_head_t h;
#define FILT_TEXTBUFSIZE (512-sizeof(struct filter_head_t))
	char textbuf[FILT_TEXTBUFSIZE];
};

#define QC_C	0x001 /* Q-filter flag bits */
#define QC_X	0x002
#define QC_U	0x004
#define QC_o	0x008
#define QC_O	0x010
#define QC_S	0x020
#define QC_r	0x040
#define QC_R	0x080
#define QC_Z	0x100
#define QC_I	0x200

#define QC_AnalyticsI	0x800

/* For q-filter analytics: entrycall igate filter database */
struct filter_entrycall_t {
	struct filter_entrycall_t *next;
	time_t expirytime;
	uint32_t hash;
	int	len;
	char	callsign[CALLSIGNLEN_MAX+1];
};

struct filter_wx_t {
	struct filter_wx_t *next;
	time_t expirytime;
	uint32_t hash;
	int	len;
	char	callsign[CALLSIGNLEN_MAX+1];
};

typedef enum {
	MatchExact,
	MatchPrefix,
	MatchWild
} MatchEnum;

#define FILTER_ENTRYCALL_HASHSIZE 2048 /* Around 500-600 in db,  this looks
					  for collision free result.. */
rwlock_t filter_entrycall_rwlock;
int filter_entrycall_maxage = 60*60;  /* 1 hour, default.  Validity on
					 lookups: 5 minutes less..  */
int filter_entrycall_cellgauge;

struct filter_entrycall_t *filter_entrycall_hash[FILTER_ENTRYCALL_HASHSIZE];


#define FILTER_WX_HASHSIZE 1024        /* Around 300-400 in db,  this looks
					  for collision free result.. */
rwlock_t filter_wx_rwlock;
int filter_wx_maxage = 60*60;         /* 1 hour, default.  Validity on
					 lookups: 5 minutes less..  */
int filter_wx_cellgauge;

struct filter_wx_t *filter_wx_hash[FILTER_WX_HASHSIZE];

rwlock_t filter_cellgauge_rwlock;
int filter_cellgauge;


#ifndef _FOR_VALGRIND_
cellarena_t *filter_cells;
cellarena_t *filter_entrycall_cells;
cellarena_t *filter_wx_cells;
#endif

int have_filtered_listeners;  /* do we have any filtered listeners, do we need to support them */

#define HIST_LOOKUP_INTERVAL 10 /* Cache historydb position lookups this much seconds on
				  each filter entry referring to some
				  fixed callsign (f,m,t) */


float filter_lat2rad(float lat)
{
	return (lat * (M_PI / 180.0));
}

float filter_lon2rad(float lon)
{
	return (lon * (M_PI / 180.0));
}


void filter_init(void)
{
	rwl_init(&filter_cellgauge_rwlock);
#ifndef _FOR_VALGRIND_
	/* A few hundred... */

	filter_cells = cellinit( "filter",
				 sizeof(struct filter_t),
				 __alignof__(struct filter_t),
				 CELLMALLOC_POLICY_LIFO,
				 512 /* 512 kB at the time,
					should be enough forever.. */,
				 0 /* minfree */ );

	/* printf("filter: sizeof=%d alignof=%d\n",
	   sizeof(struct filter_t),__alignof__(struct filter_t)); */

	/* Couple thousand */

	filter_entrycall_cells = cellinit( "entrycall",
					   sizeof(struct filter_entrycall_t),
					   __alignof__(struct filter_entrycall_t),
					   CELLMALLOC_POLICY_FIFO,
					   32 /* 32 kB at the time */,
					   0 /* minfree */ );

	/* Under 1 thousand.. */

	filter_wx_cells = cellinit( "wxcalls",
				    sizeof(struct filter_wx_t),
				    __alignof__(struct filter_wx_t),
				    CELLMALLOC_POLICY_FIFO,
				    32 /* 32 kB at the time */,
				    0 /* minfree */ );
#endif
}

static void filter_entrycall_free(struct filter_entrycall_t *f)
{
#ifndef _FOR_VALGRIND_
	cellfree( filter_entrycall_cells, f );
#else
	hfree(f);
#endif
	-- filter_entrycall_cellgauge;
}

/*
 *	filter_entrycall_insert() is for support of  q//i  filters.
 *	That is, "pass on any message that has traversed thru entry 
 *	igate which has identified itself with qAr or qAR.  Not all
 *	messages traversed thru such gate will have those same q-cons
 *	values, thus this database keeps info about entry igate that
 *	have shown such capability in recent past.
 *
 *	This must be called by the incoming_parse() in every case
 *	(or at least when qcons is either 'r' or 'R'.)
 *
 *	The key has no guaranteed alignment, no way to play tricks
 *	with gcc builtin optimizers.
 */

static int filter_entrycall_insert(struct pbuf_t *pb)
{
	struct filter_entrycall_t *f, **fp, *f2;
	/* OK, pre-parsing produced accepted result */
	uint32_t hash;
	int idx, keylen;
	const char qcons = pb->qconst_start[2];
	const char *key = pb->qconst_start+4;
        char uckey[CALLSIGNLEN_MAX+1];

	for (keylen = 0; keylen < CALLSIGNLEN_MAX; ++keylen) {
		int c = key[keylen];
		if (c == ',' || c == ':')
			break;
                if ('a' <= c && c <= 'z')
                	c -= ('a' - 'A');
                uckey[keylen] = c;
                uckey[keylen+1] = 0;
	}
	if ((key[keylen] != ',' && key[keylen] != ':') ||
	    (keylen < CALLSIGNLEN_MIN))
		return 0; /* Bad entry-station callsign */
	
	pb->entrycall_len = keylen; // FIXME: should be in incoming parser...
	
	/* We insert only those that have Q-Constructs of qAR or qAr */
	if (qcons != 'r' && qcons != 'R') return 0;

	hash = keyhash(uckey, keylen, 0);
	idx = (hash ^ (hash >> 11) ^ (hash >> 22) ) % FILTER_ENTRYCALL_HASHSIZE; /* Fold the hashbits.. */

	rwl_wrlock(&filter_entrycall_rwlock);

	fp = &filter_entrycall_hash[idx];
	f2 = NULL;
	while (( f = *fp )) {
		if ( f->hash == hash ) {
			if (f->len == keylen) {
				int cmp = memcmp(f->callsign, uckey, keylen);
				if (cmp == 0) { /* Have key match */
					f->expirytime = tick + filter_entrycall_maxage;
					f2 = f;
					break;
				}
			}
		}
		/* No match at all, advance the pointer.. */
		fp = &(f -> next);
	}
	if (!f2) {

		/* Allocate and insert into hash table */

		fp = &filter_entrycall_hash[idx];

#ifndef _FOR_VALGRIND_
		f = cellmalloc(filter_entrycall_cells);
#else
		f = hmalloc(sizeof(*f));
#endif
		if (f) {
			f->next  = *fp;
			f->expirytime = tick + filter_entrycall_maxage;
			f->hash  = hash;
			f->len   = keylen;
			memcpy(f->callsign, uckey, keylen);
			memset(f->callsign+keylen, 0, sizeof(f->callsign)-keylen);

			*fp = f2 = f;

			++ filter_entrycall_cellgauge;
		} else {
			hlog(LOG_ERR, "filter_entrycall_insert: cellmalloc failed");
		}
	}

	rwl_wrunlock(&filter_entrycall_rwlock);

	return (f2 != NULL);
}

/*
 *	filter_entrycall_lookup() is for support of  q//i  filters.
 *	That is, "pass on any message that has traversed thru entry 
 *	igate which has identified itself with qAr or qAR.  Not all
 *	messages traversed thru such gate will have those same q-cons
 *	values, thus this keeps database about entry servers that have
 *	shown such capability in recent past.
 *
 *	The key has no guaranteed alignment, no way to play tricks
 *	with gcc builtin optimizers.
 */

static int filter_entrycall_lookup(const struct pbuf_t *pb)
{
	struct filter_entrycall_t *f, **fp, *f2;
	const char *key  = pb->qconst_start+4;
	const int keylen = pb->entrycall_len;

	uint32_t  hash   = keyhashuc(key, keylen, 0);
	int idx = ( hash ^ (hash >> 11) ^ (hash >> 22) ) % FILTER_ENTRYCALL_HASHSIZE;   /* fold the hashbits.. */

	f2 = NULL;

	rwl_rdlock(&filter_entrycall_rwlock);

	fp = &filter_entrycall_hash[idx];
	while (( f = *fp )) {
		if ( f->hash == hash ) {
			if (f->len == keylen) {
				int rc =  strncasecmp(f->callsign, key, keylen);
				if (rc == 0) { /* Have key match, see if it is
						  still valid entry ? */
					if (f->expirytime < tick - 60) {
						f2 = f;
						break;
					}
				}
			}
		}
		/* No match at all, advance the pointer.. */
		fp = &(f -> next);
	}

	rwl_rdunlock(&filter_entrycall_rwlock);

	return (f2 != NULL);
}

/* 
 *	The  filter_entrycall_cleanup()  does purge old entries
 *	out of the database.  Run about once a minute.
 */
void filter_entrycall_cleanup(void)
{
	int k, cleancount = 0;
	struct filter_entrycall_t *f, **fp;

	rwl_wrlock(&filter_entrycall_rwlock);

	for (k = 0; k < FILTER_ENTRYCALL_HASHSIZE; ++k) {
		fp = & filter_entrycall_hash[k];
		while (( f = *fp )) {
			/* Did it expire ? */
			if (f->expirytime <= tick) {
				*fp = f->next;
				f->next = NULL;
				filter_entrycall_free(f);
				++cleancount;
				continue;
			}
			/* No purge, advance the pointer.. */
			fp = &(f -> next);
		}
	}

	rwl_wrunlock(&filter_entrycall_rwlock);

	// hlog( LOG_DEBUG, "filter_entrycall_cleanup() removed %d entries, count now: %ld",
	//       cleancount, filter_entrycall_cellgauge );
}

/* 
 *	The  filter_entrycall_atend()  does purge all entries
 *	out of the database.  Run at the exit of the program.
 *	This exists primarily to make valgrind happy...
 */
void filter_entrycall_atend(void)
{
	int k;
	struct filter_entrycall_t *f, **fp;

	rwl_wrlock(&filter_entrycall_rwlock);

	for (k = 0; k < FILTER_ENTRYCALL_HASHSIZE; ++k) {
		fp = & filter_entrycall_hash[k];
		while (( f = *fp )) {
			*fp = f->next;
			f->next = NULL;
			filter_entrycall_free(f);
		}
	}

	rwl_wrunlock(&filter_entrycall_rwlock);
}


void filter_entrycall_dump(FILE *fp)
{
	int k;
	struct filter_entrycall_t *f;

	rwl_rdlock(&filter_entrycall_rwlock);

	for (k = 0; k < FILTER_ENTRYCALL_HASHSIZE; ++k) {
		f = filter_entrycall_hash[k];

		for ( ; f; f = f->next ) {
			fprintf( fp, "%ld\t%s\n",
				 (long)f->expirytime, f->callsign );
		}
	}

	rwl_rdunlock(&filter_entrycall_rwlock);
}


/* ================================================================ */



static void filter_wx_free(struct filter_wx_t *f)
{
#ifndef _FOR_VALGRIND_
	cellfree( filter_wx_cells, f );
#else
	hfree(f);
#endif
	--filter_wx_cellgauge;
}

/*
 *	The  filter_wx_insert()  does lookup key storage for problem of:
 *
 *	Positionless T_WX packets want also position packets on output filters.
 */

static int filter_wx_insert(struct pbuf_t *pb)
{
	struct filter_wx_t *f, **fp, *f2;
	/* OK, pre-parsing produced accepted result */
	const char *key  = pb->data;
	const int keylen = pb->srccall_end - key;
	uint32_t hash;
	int idx;
        char uckey[CALLSIGNLEN_MAX+1] = "";

	/* If it is not a WX packet without position, we are not intrerested */
	if (!((pb->packettype & T_WX) && !(pb->flags & F_HASPOS)))
		return 0;

	for (idx = 0; idx < keylen && idx < CALLSIGNLEN_MAX; ++idx) {
		int c = key[idx];
		if (c == '>')
			break;
                if ('a' <= c && c <= 'z')
                  c -= ('a' - 'A');
                uckey[idx] = c;
                uckey[idx+1] = 0;
	}

	hash = keyhash(uckey, keylen, 0);
	idx = ( hash ^ (hash >> 10) ^ (hash >> 20) ) % FILTER_WX_HASHSIZE; /* fold the hashbits.. */

	rwl_wrlock(&filter_wx_rwlock);

	fp = &filter_wx_hash[idx];
	f2 = NULL;
	while (( f = *fp )) {
		if ( f->hash == hash ) {
			if (f->len == keylen) {
				int cmp = memcmp(f->callsign, uckey, keylen);
				if (cmp == 0) { /* Have key match */
					f->expirytime = tick + filter_wx_maxage;
					f2 = f;
					break;
				}
			}
		}
		/* No match at all, advance the pointer.. */
		fp = &(f -> next);
	}
	if (!f2) {

		/* Allocate and insert into hash table */

		fp = &filter_wx_hash[idx];

#ifndef _FOR_VALGRIND_
		f = cellmalloc(filter_wx_cells);
#else
		f = hmalloc(sizeof(*f));
#endif
		++filter_wx_cellgauge;
		if (f) {
			f->next  = *fp;
			f->expirytime = tick + filter_wx_maxage;
			f->hash  = hash;
			f->len   = keylen;
			memcpy(f->callsign, uckey, keylen);
			memset(f->callsign+keylen, 0, sizeof(f->callsign)-keylen);

			*fp = f2 = f;
		} else {
			hlog(LOG_ERR, "filter_wx_insert: cellmalloc failed");
		}
	}

	rwl_wrunlock(&filter_wx_rwlock);

	return 0;
}

static int filter_wx_lookup(const struct pbuf_t *pb)
{
	struct filter_wx_t *f, **fp, *f2;
	const char *key  = pb->data;
	const int keylen = pb->srccall_end - key;

	uint32_t  hash   = keyhashuc(key, keylen, 0);
	int idx = ( hash ^ (hash >> 10) ^ (hash >> 20) ) % FILTER_WX_HASHSIZE; /* fold the hashbits.. */

	f2 = NULL;

	rwl_rdlock(&filter_wx_rwlock);

	fp = &filter_wx_hash[idx];
	while (( f = *fp )) {
		if ( f->hash == hash ) {
			if (f->len == keylen) {
				int rc =  strncasecmp(f->callsign, key, keylen);
				if (rc == 0) { /* Have key match, see if it is
						  still valid entry ? */
					if (f->expirytime < tick - 60) {
						f2 = f;
						break;
					}
				}
			}
		}
		/* No match at all, advance the pointer.. */
		fp = &(f -> next);
	}

	rwl_rdunlock(&filter_wx_rwlock);

	return (f2 != NULL);
}


/* 
 *	The  filter_wx_cleanup()  does purge old entries
 *	out of the database.  Run about once a minute.
 */
void filter_wx_cleanup(void)
{
	int k, cleancount = 0;
	struct filter_wx_t *f, **fp;

	rwl_wrlock(&filter_wx_rwlock);

	for (k = 0; k < FILTER_WX_HASHSIZE; ++k) {
		fp = & filter_wx_hash[k];
		while (( f = *fp )) {
			/* Did it expire ? */
			if (f->expirytime <= tick) {
				*fp = f->next;
				f->next = NULL;
				filter_wx_free(f);
				++cleancount;
				continue;
			}
			/* No purge, advance the pointer.. */
			fp = &(f -> next);
		}
	}

	rwl_wrunlock(&filter_wx_rwlock);

	// hlog( LOG_DEBUG, "filter_wx_cleanup() removed %d entries, count now: %ld",
	//       cleancount, filter_wx_cellgauge );
}

/* 
 *	The  filter_wx_atend()  does purge all entries
 *	out of the database.  Run at the exit of the program.
 *	This exists primarily to make valgrind happy...
 */
void filter_wx_atend(void)
{
	int k;
	struct filter_wx_t *f, **fp;

	rwl_wrlock(&filter_wx_rwlock);

	for (k = 0; k < FILTER_WX_HASHSIZE; ++k) {
		fp = & filter_wx_hash[k];
		while (( f = *fp )) {
			*fp = f->next;
			f->next = NULL;
			filter_wx_free(f);
		}
	}

	rwl_wrunlock(&filter_wx_rwlock);
}


void filter_wx_dump(FILE *fp)
{
	int k;
	struct filter_wx_t *f;

	rwl_rdlock(&filter_wx_rwlock);

	for (k = 0; k < FILTER_WX_HASHSIZE; ++k) {
		f = filter_wx_hash[k];

		for ( ; f; f = f->next ) {
			fprintf( fp, "%ld\t%s\n",
				 (long)f->expirytime, f->callsign );
		}
	}

	rwl_rdunlock(&filter_wx_rwlock);
}


/* ================================================================ */

const char *aprsc_strnstr(const char *s1, const char *s2, size_t len)
{
	char  c1, c2;
	size_t  n;
	
	c2 = *(char *) s2++;
	
	n = strlen(s2);
	
	do {
		do {
			/* if we're going too far */
			if (len-- == 0)
				return NULL;
			
			/* pick first char and advance */
			c1 = *s1++;
			
			/* end of string? */
			if (c1 == 0)
				return NULL;
		} while (c1 != c2);
		
		/* at least the first character matches... if the needle is longer than
		 * what we have left in the haystack, we won't find a match.
		 */
		
		if (n > len)
			return NULL;
		
	} while (strncmp(s1, s2, n) != 0);
	
	return s1;
}


/*
 *	Check if there is a TCPIP* component in the path.
 *	OPTIMIZE: The flag could be set when TCPIP is inserted, or when the path
 *	is analyzed on input.
 */

static void filter_check_tcpip(struct pbuf_t *pbuf)
{
	if (aprsc_strnstr(pbuf->dstcall_end, ",TCPIP*,", pbuf->qconst_start - pbuf->dstcall_end))
		pbuf->flags |= F_HAS_TCPIP;
}

static void filter_keyhashes(struct pbuf_t *pb)
{
	pb->srccall_hash = keyhashuc(pb->data, pb->srccall_end - pb->data, 0);
	
	if (pb->srcname == pb->data)
		pb->srcname_hash = pb->srccall_hash;
	else
		pb->srcname_hash = keyhashuc(pb->srcname, pb->srcname_len, 0);
		
	if (pb->dstname)
		pb->dstname_hash = keyhashuc(pb->dstname, pb->dstname_len, 0);
}

void filter_preprocess_dupefilter(struct pbuf_t *pbuf)
{
	// TODO: could probably skip filter_check_tcpip, and possibly filter_keyhashes too if no filtered listeners
	filter_check_tcpip(pbuf);
	if (have_filtered_listeners) {
		filter_entrycall_insert(pbuf);
		filter_wx_insert(pbuf);
	}
	filter_keyhashes(pbuf);
}

void filter_postprocess_dupefilter(struct pbuf_t *pbuf)
{
	/*
	 *    If there is no position at this packet from earlier
	 *    processing, try now to find one by the callsign of
	 *    the packet sender.
	 *    
	 */
	if (!(pbuf->flags & F_HASPOS)) {
		struct history_cell_t *hist;
		int rc = historydb_lookup(pbuf->srcname, pbuf->srcname_len, &hist);
		// hlog( LOG_DEBUG, "postprocess_dupefilter: no pos, looking up '%.*s', rc=%d",
		//       pbuf->srcname_len, pbuf->srcname, rc );
		if (rc > 0) {
			pbuf->lat     = hist->lat;
			pbuf->lng     = hist->lon;
			pbuf->cos_lat = hist->coslat;

			pbuf->flags  |= F_HASPOS;
		}
	}
}


/* ================================================================ */

/*
 *	filter_match_on_callsignset()  matches prefixes, or exact keys
 *	on filters of types:  b, d, e, o, p, u
 *	('p' and 'b' need OPTIMIZATION - others get it for free)
 *
 */

static int filter_match_on_callsignset(struct filter_refcallsign_t *ref, int keylen, struct filter_t *f, MatchEnum wildok)
{
	int i;
	struct filter_refcallsign_t *r  = f->h.refcallsigns;
	const char                  *r1 = (const void*)ref->callsign;

	for (i = 0; i < f->h.numnames; ++i) {
		const int reflen = r[i].reflen;
		const int len    = reflen & LengthMask;
		const char   *r2 = (const void*)r[i].callsign;

		switch (wildok) {
		case MatchExact:
			if (len != keylen)
				continue; /* no match */
			/* length OK, compare content */
			if (strncasecmp( r1, r2, len ) != 0) continue;
			/* So it was an exact match
			** Precisely speaking..  we should check that there is
			** no WildCard flag, or such.  But then this match
			** method should not be used if parser finds any such.
			*/
			return ( reflen & NegationFlag ? 2 : 1 );
			break;
		case MatchPrefix:
			if (len > keylen || !len) {
			/* reference string length is longer than our key */
				continue;
			}
			if (strncasecmp( r1, r2, len ) != 0) continue;

			return ( reflen & NegationFlag ? 2 : 1 );
			break;
		case MatchWild:
			if (len > keylen || !len) {
			/* reference string length is longer than our key */
				continue;
			}

			if (strncasecmp( r1, r2, len ) != 0) continue;

			if (reflen & WildCard)
				return ( reflen & NegationFlag ? 2 : 1 );

			if (len == keylen)
				return ( reflen & NegationFlag ? 2 : 1 );
			break;
		default:
			break;
		}
	}
	return 0; /* no match */

}

/*
 *	filter_parse_one_callsignset()  collects multiple callsigns
 *	on filters of types:  b, d, e, o, p, u
 *
 *	If previous filter was of same type as this one, that one's
 *	refbuf is extended.
 */

static int filter_parse_one_callsignset(struct client_t *c, const char *filt0, struct filter_t *f0, struct filter_t *ff, struct filter_t **ffp, MatchEnum wildok)
{
	char prefixbuf[CALLSIGNLEN_MAX+1];
	char *k;
	const char *p;
	int i, refcount, wildcard;
	int refmax = 0, extend = 0;
	struct filter_refcallsign_t *refbuf;
	
	p = filt0;
	if (*p == '-') ++p;
	while (*p && *p != '/') ++p;
	if (*p == '/') ++p;
	/* count the number of prefixes in there.. */
	while (*p) {
		if (*p) ++refmax;
		while (*p && *p != '/') ++p;
		if (*p == '/') ++p;
	}
	if (refmax == 0) return -1; /* No prefixes ?? */

	if (ff && ff->h.type == f0->h.type) { /* SAME TYPE,
						 extend previous record! */
		extend = 1;
		refcount = ff->h.numnames + refmax;
		//hlog(LOG_DEBUG, "extending, refcount %d numnames %d refmax %d", refcount, ff->h.numnames, refmax);
		refbuf   = hrealloc(ff->h.refcallsigns, sizeof(*refbuf) * refcount);
		ff->h.refcallsigns = refbuf;
		refcount = ff->h.numnames;
	} else {
		refbuf = hmalloc(sizeof(*refbuf)*refmax);
		refcount = 0;
		f0->h.refcallsigns = refbuf;
		f0->h.numnames     = 0;
	}

	p = filt0;
	if (*p == '-') ++p;
	while (*p && *p != '/') ++p;
	if (*p == '/') ++p;

	//hlog(LOG_DEBUG, "p-filter: '%s'", p);
	while (*p)  {
		k = prefixbuf;
		memset(prefixbuf, 0, sizeof(prefixbuf));
		i = 0;
		wildcard = 0;
		/* loop through the whole callsign, but only copy max CALLSIGNLEN_MAX
		 * bytes to the buffer
		 */
		while (*p != 0 && *p != '/') {
			if (*p == '*') {
				wildcard = 1;
				++p;
				if (wildok != MatchWild)
					return -1;
				continue;
			}
			if (i < CALLSIGNLEN_MAX) {
				*k = *p;
				++k;
			}
			++p;
			++i;
		}
		*k = 0;
		
		/* OK, we have one prefix part collected,
		   scan source until next '/' */
		if (*p != 0 && *p != '/') ++p;
		if (*p == '/') ++p;
		
		/* If there is more of patterns, the loop continues.. */

		/* Store the refprefix */
		memset(&refbuf[refcount], 0, sizeof(refbuf[refcount]));
		memcpy(refbuf[refcount].callsign, prefixbuf, sizeof(refbuf[refcount].callsign));
		refbuf[refcount].reflen = strlen(prefixbuf);
		if (wildcard)
			refbuf[refcount].reflen |= WildCard;
		if (f0->h.negation)
			refbuf[refcount].reflen |= NegationFlag;
		++refcount;
	}

	f0->h.numnames = refcount;
	
	if (extend) {
		char *s;
		ff->h.numnames     = refcount;
		i = strlen(ff->h.text) + strlen(filt0)+2;
		if (i <= FILT_TEXTBUFSIZE) {
			/* Fits in our built-in buffer block - like previous..
			** Append on existing buffer
			*/
			s = ff->textbuf + strlen(ff->textbuf);
			sprintf(s, " %s", filt0);
		} else {
			/* It does not fit anymore.. */
			s = hmalloc(i); /* alloc a new one */
			sprintf(s, "%s %s", ff->textbuf, filt0); /* .. and catenate. */
			p = ff->h.text;
			if (ff->h.text != ff->textbuf) /* possibly free old */
				hfree((void*)p);
			ff->h.text = s;     /* store new */
		}
	}
	/* If not extending existing filter item,
	   let main parser do the finalizations */

	return extend;
}

static int filter_parse_one_s(struct client_t *c, const char *filt0, struct filter_t *f0, struct filter_t *ff, struct filter_t **ffp)
{
	/* s/pri/alt/over  	Symbol filter

	   pri = symbols in primary table
	   alt = symbols in alternate table
	   over = overlay character (case sensitive)

	   For example:
	   s/->   This will pass all House and Car symbols (primary table)
	   s//#   This will pass all Digi with or without overlay
	   s//#/T This will pass all Digi with overlay of capital T

	   About 10-15 s-filters in entire APRS-IS core at any given time.
	   Up to 520 invocations per second at peak.
	*/
	const char *s = filt0;
	int len1, len2, len3, len4, len5, len6;

	if (*s == '-')
		++s;
	if (*s == 's' || *s == 'S')
		++s;
	if (*s != '/')
		return -1; 
	++s;

	len1 = len2 = len3 = len4 = len5 = len6 = 0;
	
	while (1) {
		len1 = s - filt0;
		while (*s && *s != '/')
			s++;
		len2 = s - filt0;

		f0->h.len1s = len1;
		f0->h.len1  = len2 - len1;
		f0->h.lens.len2s = f0->h.lens.len2 = f0->h.lens.len3s = f0->h.lens.len3 = 0;

		if (!*s) break;

		if (*s == '/') ++s;
		len3 = s - filt0;
		while (*s && *s != '/')
			s++;
		len4 = s - filt0;

		f0->h.lens.len2s = len3;
		f0->h.lens.len2  = len4 - len3;

		if (!*s) break;

		if (*s == '/') ++s;
		len5 = s - filt0;
		while (*s) ++s;
		len6 = s - filt0;

		f0->h.lens.len3s = len5;
		f0->h.lens.len3  = len6 - len5;

		break;
	}

	if ((len6-len5 > 0) && (len4-len3 == 0)) {
		/* overlay but no secondary table.. */
		return -1; /* bad parse */
	}
#if 0
	{
	  const char  *s1 = filt0+len1, *s2 = filt0+len3, *s3 = filt0+len5;
	  int l1 = len2-len1, l2 = len4-len3, l3 = len6-len5;

	  hlog( LOG_DEBUG, "parse s-filter:  '%.*s'  '%.*s'  '%.*s'",
		l1, s1, l2, s2, l3, s3 );
	}
#endif
	return 0;
}


int filter_parse(struct client_t *c, const char *filt, int is_user_filter)
{
	struct filter_t *f, f0;
	int i;
	const char *filt0 = filt;
	const char *s;
	char dummyc;
	struct filter_t *fp, *fn, *ff, **fff, **ffp, **ffn;

	if (is_user_filter && c && (!(c->flags & CLFLAGS_USERFILTEROK))) {
		hlog(LOG_DEBUG, "No user-specified filters permitted");
		return -1;
	}

	if (!c) { /* built-in configuration scanning filter parsing */
		ffp = &f;
		ffn = &f;
		f = NULL;
	} else if (is_user_filter) {
		ffn = &c->neguserfilters;
		ffp = &c->posuserfilters;
	} else {
		ffn = &c->negdefaultfilters;
		ffp = &c->posdefaultfilters;
	}

	fp = *ffp;
	fn = *ffn;
	for ( ; fp && fp->h.next; fp = fp->h.next)
	  ;
	for ( ; fn && fn->h.next; fn = fn->h.next)
	  ;
	/* f[np]  points to last so far accumulated filter,
	   if none were previously received, it is NULL.. */

	memset(&f0, 0, sizeof(f0));
	if (*filt == '-') {
		++filt;
		ff  = fn;
		fff = ffn;
	} else {
		ff  = fp;
		fff = ffp;
	}
	f0.h.type = *filt;

	if (!strchr("abdefgmopqrstuABDEFGMOPQRSTU", *filt)) {
		/* Not valid filter code */
		hlog(LOG_DEBUG, "Bad filter code: %s", filt0);
		return -1;
	}

	switch (f0.h.type) {
	case 'a':
	case 'A':
		/*  a/latN/lonW/latS/lonE     Area filter -- OPTIMIZE!  */

		i = sscanf(filt+1, "/%f/%f/%f/%f%c",
			   &f0.h.f_latN, &f0.h.f_lonW,
			   &f0.h.f_latS, &f0.h.f_lonE, &dummyc);

		if (i == 5 && dummyc == '/')
			i = 4;

		if (i != 4) {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}

		if (!( -90.01 < f0.h.f_latN && f0.h.f_latN <  90.01)) {
			hlog(LOG_DEBUG, "Bad filter latN value: %s", filt0);
			return -2;
		}
		if (!(-180.01 < f0.h.f_lonW && f0.h.f_lonW < 180.01)) {
			hlog(LOG_DEBUG, "Bad filter lonW value: %s", filt0);
			return -2;
		}
		if (!( -90.01 < f0.h.f_latS && f0.h.f_latS <  90.01)) {
			hlog(LOG_DEBUG, "Bad filter latS value: %s", filt0);
			return -2;
		}
		if (!(-180.01 < f0.h.f_lonE && f0.h.f_lonE < 180.01)) {
			hlog(LOG_DEBUG, "Bad filter lonE value: %s", filt0);
			return -2;
		}

		if (f0.h.f_latN < f0.h.f_latS) {
			hlog(LOG_DEBUG, "Bad filter: latN<latS: %s", filt0);
			return -3; /* expect: latN >= latS */
		}
		if (f0.h.f_lonW > f0.h.f_lonE) {
			hlog(LOG_DEBUG, "Bad filter: lonW>lonE: %s", filt0);
			return -3; /* expect: lonW <= lonE */
		}

		// hlog(LOG_DEBUG, "Filter: %s -> A %.3f %.3f %.3f %.3f",
		//      filt0, f0.h.f_latN, f0.h.f_lonW,
		//      f0.h.f_latS, f0.h.f_lonE);
		
		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonW = filter_lon2rad(f0.h.f_lonW);
		
		f0.h.f_latS = filter_lat2rad(f0.h.f_latS);
		f0.h.f_lonE = filter_lon2rad(f0.h.f_lonE);

		

		break;

	case 'b':
	case 'B':
		/*  b/call1/call2...   Budlist filter (*) */

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;


		break;

	case 'd':
	case 'D':
		/* d/digi1/digi2...  	Used digipeater filter (*)	*/

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;

		break;

	case 'e':
	case 'E':
		/*   e/call1/call1/...  Entry station filter (*) */

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;

		break;

	case 'f':
	case 'F':
		/*  f/call/dist         Friend's range filter  */

		i = sscanf(filt+1, "/%9[^/]/%f", f0.h.refcallsign.callsign, &f0.h.f_dist);
		if (i != 2 || f0.h.f_dist < 0.1) {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}

		f0.h.refcallsign.callsign[CALLSIGNLEN_MAX] = 0;
		f0.h.refcallsign.reflen = strlen(f0.h.refcallsign.callsign);
		f0.h.numnames = 0; /* reusing this as "position-cache valid" flag */

		// hlog(LOG_DEBUG, "Filter: %s -> F xxx %.3f", filt0, f0.h.f_dist);

		/* NOTE: Could do static location resolving at connect time, 
		** and then use the same way as 'r' range does.  The friends
		** are rarely moving...
		*/

		break;

	case 'g':
	case 'G':
		/*   g/call1/call1/...  Text message destination filter (*) */

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;

		break;

	case 'm':
	case 'M':
		/*  m/dist            My range filter  */

		i = sscanf(filt+1, "/%f", &f0.h.f_dist);
		if (i != 1 || f0.h.f_dist < 0.1) {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}
		f0.h.numnames = 0; /* reusing this as "position-cache valid" flag */

		// hlog(LOG_DEBUG, "Filter: %s -> M %.3f", filt0, f0.h.f_dist);
		break;

	case 'o':
	case 'O':
		/* o/obje1/obj2...  	Object filter (*)	*/

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;

		break;

	case 'p':
	case 'P':
		/* p/aa/bb/cc...  	Prefix filter
		   Pass traffic with fromCall that start with aa or bb or cc...
		*/
		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchPrefix );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;

		break;

	case 'q':
	case 'Q':
		/* q/con/ana           q Contruct filter */
		s = filt+1;
		f0.h.type = 'q';
		f0.h.bitflags = 0; /* For QC_*  flags */

		if (*s++ != '/') {
			hlog(LOG_DEBUG, "Bad q-filter parse: %s", filt0);
			return -1;
		}
		for ( ; *s && *s != '/'; ++s ) {
			switch (*s) {
			case 'C':
				f0.h.bitflags |= QC_C;
				break;
			case 'X':
				f0.h.bitflags |= QC_X;
				break;
			case 'U':
				f0.h.bitflags |= QC_U;
				break;
			case 'o':
				f0.h.bitflags |= QC_o;
				break;
			case 'O':
				f0.h.bitflags |= QC_O;
				break;
			case 'S':
				f0.h.bitflags |= QC_S;
				break;
			case 'r':
				f0.h.bitflags |= QC_r;
				break;
			case 'R':
				f0.h.bitflags |= QC_R;
				break;
			case 'Z':
				f0.h.bitflags |= QC_Z;
				break;
			case 'I':
				f0.h.bitflags |= QC_I;
				break;
			default:
				hlog(LOG_DEBUG, "Bad q-filter parse: %s", filt0);
				return -1;
			}
		}
		if (*s == '/') { /* second format */
			++s;
			if (*s == 'i' || *s == 'I') {
				f0.h.bitflags |= QC_AnalyticsI;
				++s;
			}
			if (*s) {
				hlog(LOG_DEBUG, "Bad q-filter parse: %s", filt0);
				return -1;
			}
		}
		
		break;

	case 'r':
	case 'R':
		/*  r/lat/lon/dist            Range filter  */

		i = sscanf(filt+1, "/%f/%f/%f",
			 &f0.h.f_latN, &f0.h.f_lonE, &f0.h.f_dist);
		if (i != 3 || f0.h.f_dist < 0.1) {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}

		if (!( -90.01 < f0.h.f_latN && f0.h.f_latN <  90.01)) {
			hlog(LOG_DEBUG, "Bad filter lat value: %s", filt0);
			return -2;
		}
		if (!(-180.01 < f0.h.f_lonE && f0.h.f_lonE < 180.01)) {
			hlog(LOG_DEBUG, "Bad filter lon value: %s", filt0);
			return -2;
		}

		// hlog(LOG_DEBUG, "Filter: %s -> R %.3f %.3f %.3f", filt0, f0.h.f_latN, f0.h.f_lonW, f0.h.f_dist);

		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonE = filter_lon2rad(f0.h.f_lonE);

		f0.h.f_coslat = cosf( f0.h.f_latN ); /* Store pre-calculated COS of LAT */
		break;

	case 's':
	case 'S':
		/* s/pri/alt/over  	Symbol filter  */

		i = filter_parse_one_s( c, filt0, &f0, ff, fff );
		if (i < 0) {
			hlog(LOG_DEBUG, "Bad s-filter syntax: %s", filt0);
			return i;
		}
		if (i > 0) /* extended previous */
			return 0;
		break;

	case 't':
	case 'T':
		/* t/..............
		   t/............../call/km
		*/
		s = filt+1;
		f0.h.type = 't';
		f0.h.bitflags = 0;
		f0.h.numnames = 0; /* reusing this as "position-cache valid" flag */

		if (*s++ != '/') {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}
		for ( ; *s && *s != '/'; ++s ) {
			switch (*s) {
			case '*':
				f0.h.bitflags |= ~T_CWOP; /* "ALL" -- excluding CWOP */
				break;
			case 'c': case 'C':
				f0.h.bitflags |= T_CWOP;
				break;
			case 'i': case 'I':
				f0.h.bitflags |= T_ITEM;
				break;
			case 'm': case 'M':
				f0.h.bitflags |= T_MESSAGE;
				break;
			case 'n': case 'N':
				f0.h.bitflags |= T_NWS;
				break;
			case 'o': case 'O':
				f0.h.bitflags |= T_OBJECT;
				break;
			case 'p': case 'P':
				f0.h.bitflags |= T_POSITION;
				break;
			case 'q': case 'Q':
				f0.h.bitflags |= T_QUERY;
				break;
			case 's': case 'S':
				f0.h.bitflags |= T_STATUS;
				break;
			case 't': case 'T':
				f0.h.bitflags |= T_TELEMETRY;
				break;
			case 'u': case 'U':
				f0.h.bitflags |= T_USERDEF;
				break;
			case 'w': case 'W':
				f0.h.bitflags |= T_WX;
				break;
			default:
				hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
				return -1;
			}
		}
		if (*s == '/' && s[1] != 0) { /* second format */
			i = sscanf(s, "/%9[^/]/%f%c", f0.h.refcallsign.callsign, &f0.h.f_dist, &dummyc);
			if ( i != 2 || f0.h.f_dist < 0.1 || /* 0.1 km minimum radius */
			     strlen(f0.h.refcallsign.callsign) < CALLSIGNLEN_MIN ) {
				hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
				return -1;
			}
			f0.h.refcallsign.callsign[CALLSIGNLEN_MAX] = 0;
			f0.h.refcallsign.reflen = strlen(f0.h.refcallsign.callsign);
			f0.h.type = 'T'; /* two variants... */
		}

		break;

	case 'u':
	case 'U':
		/* u/unproto1/unproto2...  	Unproto filter (*)	*/

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, fff, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) /* extended previous */
			return 0;

		break;



	default:;
		/* No pre-parsers for other types */
		// hlog(LOG_DEBUG, "Filter: %s", filt0);
		break;
	}

	if (!c) return 0; /* Just a verification scan, not actual fill in parse */
	
	/* OK, pre-parsing produced accepted result */
#ifndef _FOR_VALGRIND_
	f = cellmalloc(filter_cells);
	if (!f) {
		hlog(LOG_ERR, "filter_parse: cellmalloc failed");
		return -1;
	}
	*f = f0; /* store pre-parsed values */
	if (strlen(filt0) < FILT_TEXTBUFSIZE) {
		strcpy(f->textbuf, filt0);
		f->h.text = f->textbuf;
	} else
		f->h.text = hstrdup(filt0); /* and copy of filter text */
#else
	f = hmalloc(sizeof(*f) + strlen(filt0));
	*f = f0; /* store pre-parsed values */
	f->h.text = f->textbuf;
	strcpy(f->textbuf, filt); /* and copy of filter text */
#endif
	rwl_wrlock(&filter_cellgauge_rwlock);
	++ filter_cellgauge;
	rwl_wrunlock(&filter_cellgauge_rwlock);

	hlog(LOG_DEBUG, "parsed filter: t=%c n=%d '%s'", f->h.type, f->h.negation, f->h.text);

	/* link to the tail.. */
	if (ff)
		fff = &ff->h.next;

	*fff = f;

	return 0;
}

/* Discard the defined filter chain */
void filter_free(struct filter_t *f)
{
	struct filter_t *fnext;
	int freed = 0;

	for ( ; f ; f = fnext ) {
		fnext = f->h.next;
		/* If not pointer to internal string, free it.. */
#ifndef _FOR_VALGRIND_
		if (f->h.text != f->textbuf)
			hfree((void*)(f->h.text));
		cellfree(filter_cells, f);
#else
		hfree(f);
#endif
		freed++;
	}
	
	rwl_wrlock(&filter_cellgauge_rwlock);
	filter_cellgauge -= freed;
	rwl_wrunlock(&filter_cellgauge_rwlock);
}


/*

#
# Input:  This[La]      Source Latitude, in radians
#         This[Lo]      Source Longitude, in radians
#         That[La]      Destination Latitude, in radians
#         That[Lo]      Destination Longitude, in radians
# Output: R[s]          Distance, in kilometers
#

function maidenhead_km_distance($This, $That) {

    #Haversine Formula (from R.W. Sinnott, "Virtues of the Haversine", 
    #Sky and Telescope, vol. 68, no. 2, 1984, p. 159): 

    $dlon = $That[Lo] - $This[Lo];
    $dlat = $That[La] - $This[La];

    $sinDlat2 = sin($dlat/2);
    $sinDlon2 = sin($dlon/2);
    $a = ($sinDlat2 * $sinDlat2 +
          cos($This[La]) * cos($That[La]) * $sinDlon2 * $sinDlon2);

    # The Haversine Formula can be expressed in terms of a two-argument 
    # inverse tangent function, atan2(y,x), instead of an inverse sine 
    # as follows (no bulletproofing is needed for an inverse tangent): 

    $c = 2.0 * atan2( sqrt($a), sqrt(1.0-$a) );
    # $d = R * $c ; # Radius of ball times angle [radians] ...


    $R[s] = rad2deg($c) * 111.2;

    return($R);

}

*/

static float maidenhead_km_distance(float lat1, float coslat1, float lon1, float lat2, float coslat2, float lon2)
{
	float sindlat2 = sinf((lat1 - lat2) * 0.5);
	float sindlon2 = sinf((lon1 - lon2) * 0.5);

	float a = (sindlat2 * sindlat2 +
		   coslat1 * coslat2 * sindlon2 * sindlon2);

	float c = 2.0 * atan2f( sqrtf(a), sqrtf(1.0 - a));

	return ((111.2 * 180.0 / M_PI) * c);
}


/*
 *
 *  http://www.aprs-is.net/javaprssrvr/javaprsfilter.htm
 *
 */

static int filter_process_one_a(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* a/latN/lonW/latS/lonE  	Area filter

	   The area filter works the same as range filter but the filter
	   is defined as a box of coordinates. The coordinates can also
	   been seen as upper left coordinate and lower right. Lat/lon
	   are decimal degrees.   South and west are negative.

	   Multiple area filters can be defined at the same time.

	   Messages addressed to stations within the area are also passed.
	   (by means of aprs packet parse finding out the location..)

	   50-70 instances in APRS-IS core at any given time.
	   Up to 2500 invocations per second.
	*/
	;
	if (!(pb->flags & F_HASPOS)) /* packet with a position.. (msgs with RECEIVER's position) */
		return 0;

	if ((pb->lat <= f->h.f_latN) &&
	    (pb->lat >= f->h.f_latS) &&
	    (pb->lng <= f->h.f_lonE) && /* East POSITIVE ! */
	    (pb->lng >= f->h.f_lonW))
		/* Inside the box */
		return f->h.negation ? 2 : 1;

	return 0;
}

static int filter_process_one_b(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* b/call1/call2...  	Budlist filter

	   Pass all traffic FROM exact call: call1, call2, ...
	   (* wild card allowed)

	   50/70 instances in APRS-IS core at any given time.
	   Up to 2500 invocations per second.
	*/

	struct filter_refcallsign_t ref;
	int i = pb->srccall_end - pb->data;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	/* source address  "addr">... */
	memcpy( ref.callsign, pb->data, i);
	memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

	int r = filter_match_on_callsignset(&ref, i, f, MatchWild);
	
	/* match 3rd-party packets based on their innermost srccall,
	 * only works on non-object/item currently
	 */
	if (r == 0 && pb->srcname != pb->data && (pb->packettype & (T_OBJECT|T_ITEM)) == 0) {
		i = pb->srcname_len;
		if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

		memcpy( ref.callsign, pb->srcname, i);
		memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

		return filter_match_on_callsignset(&ref, i, f, MatchWild);
	}
	
	return r;
}

static int filter_process_one_d(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* d/digi1/digi2...  	Digipeater filter

	   The digipeater filter will pass all packets that have been
	   digipeated by a particular station(s) (the station's call
	   is in the path, and a * is present in that callsign or
	   a following callsign before the Q construct).
	   This filter allows the * wildcard.

	   25-35 filters in use at any given time.
	   Up to 1300 invocations per second.
	*/
	struct filter_refcallsign_t ref;
	const char *d = pb->srccall_end + 1 + pb->dstcall_len + 1; /* viacall start */
	const char *q = pb->qconst_start-1;
	int rc, i, cl, j = 0;
	int found_call = 0;

	/*hlog( LOG_INFO, "digifilter:  '%.*s' -> '%.*s'  q-d=%d",
	 *      (int)(pb->packet_len < 50 ? pb->packet_len : 50),
	 *      pb->data, (int)i, d, (int)(q-d) );
	 */
	
	for (i = 0; d < q; ) {
		++j;
		if (j > 10) break; /* way too many callsigns... */

		if (*d == ',') ++d; /* second round and onwards.. */
		/* find end of callsign */
		for (i = 0; i+d <= q && i <= CALLSIGNLEN_MAX; ++i) {
			if (d[i] == ',')
				break;
		}

		//hlog(LOG_INFO, "d:  -> (%d,%d) '%.*s'", (int)(d-pb->data), i, i, d);
		
		/* When matching callsign, ignore trailing '*' */
		cl = i;
		if (d[cl-1] == '*')
			cl--;

		if (cl > CALLSIGNLEN_MAX) cl = CALLSIGNLEN_MAX;
		
		/* digipeater address  ",addr," */
		memcpy( ref.callsign, d, cl);
		memset( ref.callsign+cl, 0, sizeof(ref.callsign)-cl );

		rc = filter_match_on_callsignset(&ref, cl, f, MatchWild);
		if (rc) {
			if (rc == 1) {
				found_call = 1;
				break;
			}
		}
		d += i;
	}
	
	if (!found_call)
		return 0;
	
	/* Ok, we found the call. Check if it has a '*' in the end.
	 * Or if one of the following calls does.
	 *
	 * d points to the start of this call, d[i-1] should be it's last character.
	 */
	
	if (d[cl] == '*')
		return 1;
	
	/* continue looking from next callsign */
	d += i;
	while (d < q) {
		++j;
		if (j > 10) break; /* way too many callsigns... */

		if (*d == ',')
			d++;
		
		/* find end of callsign */
		for (i = 0; i+d <= q && i <= CALLSIGNLEN_MAX; ++i) {
			if (d[i] == ',')
				break;
		}
		
		//hlog(LOG_INFO, "d:  -> (%d,%d) '%.*s'", (int)(d-pb->data), i, i, d);
		
		/* Is there a trailing '*' in the call? */
		if (d[i-1] == '*')
			return 1;;
		
		d += i;
	}
	
	return 0;
}

static int filter_process_one_e(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* e/call1/call1/...  	Entry station filter

	   This filter passes all packets with the specified
	   callsign-SSID(s) immediately following the q construct.
	   This allows filtering based on receiving IGate, etc.
	   Supports * wildcard.

	   2-6 instances in APRS-IS core at any given time.
	   Up to 200 invocations per second.
	*/

	struct filter_refcallsign_t ref;
	const char *e = pb->qconst_start+4;
	int         i = pb->entrycall_len;

	if (i < 1) /* should not happen.. */
		return 0; /* Bad Entry-station callsign */

	/* entry station address  "qA*,addr," */
	memcpy( ref.callsign, e, i);
	memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

	return filter_match_on_callsignset(&ref, i, f, MatchWild);
}

static int filter_process_one_f(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* f/call/dist  	Friend Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of call.

	   Multiple friend filters can be defined at the same time.

	   Messages addressed to stations within the range are also passed.
	   (by means of aprs packet parse finding out the location..)

	   NOTE: Could do static location resolving at connect time, 
	   and then use the same way as 'r' range does.  The friends
	   are rarely moving...

	   15-25 instances in APRS-IS core at any given time.
	   Up to 900 invocations per second.

	   Caching the historydb_lookup() result will lower CPU power
	   spent on the historydb.
	*/

	struct history_cell_t *history;

	float r;
	float lat1, lon1, coslat1;
	float lat2, lon2, coslat2;

	const char *callsign = f->h.refcallsign.callsign;
	int i                = f->h.refcallsign.reflen;

	if (!(pb->flags & F_HASPOS)) /* packet with a position.. (msgs with RECEIVER's position) */
		return 0; /* No position data... */

	/* find friend's last location packet */
	if (f->h.hist_age < tick || f->h.hist_age > tick + HIST_LOOKUP_INTERVAL) {
		i = historydb_lookup( callsign, i, &history );
		f->h.numnames = i;
		f->h.hist_age = tick + HIST_LOOKUP_INTERVAL;
		if (!i) return 0; /* no lookup result.. */
		f->h.f_latN   = history->lat;
		f->h.f_lonE   = history->lon;
		f->h.f_coslat = history->coslat;
	}
	if (!f->h.numnames) return 0; /* histdb lookup cache invalid */

	lat1    = f->h.f_latN;
	lon1    = f->h.f_lonE;
	coslat1 = f->h.f_coslat;

	lat2    = pb->lat;
	lon2    = pb->lng;
	coslat2 = pb->cos_lat;

	r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);

	if (r < f->h.f_dist)  /* Range is less than given limit */
		return (f->h.negation) ? 2 : 1;

	return 0;
}

static int filter_process_one_g(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* g/call1/call1/...  	Test message recipient callsign filter

	   This filter passes all packets with the specified
	   callsign-SSID(s) appearing as the text message
	   recipient. Wildcards supported.

	   Appeared in javAPRSSrvr 4.0, not widely used.
	*/

	if ( (pb->packettype & T_MESSAGE) == 0 ) /* not a message */
		return 0;

	struct filter_refcallsign_t ref;
	const char *e = pb->dstname;
	int         i = pb->dstname_len;

	if (i < 1) /* should not happen.. */
		return 0; /* Bad Entry-station callsign */

	memcpy( ref.callsign, e, i);
	memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

	return filter_match_on_callsignset(&ref, i, f, MatchWild);
}

static int filter_process_one_m(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* m/dist  	My Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of the logged in client.
	   A client location stored in the client struct is used instead of
	   the history database, since the filter should work for non-validated
	   clients (such as aprsdroid new users which have not received their
	   passcode quite yet). The historydb may only be updated by packets
	   which are actually forwarded.

	   Messages addressed to stations within the range are also passed.
	   (by means of aprs packet parse finding out the location..)

	   NOTE:  MY RANGE is rarely moving, once there is a positional
	   fix, it could stay fixed...

	   80-120 instances in APRS-IS core at any given time.
	   Up to 4200 invocations per second.
	   
	   Caching the historydb_lookup() result will lower CPU power
	   spent on the historydb.
	*/

	float r;
	float lat1, lon1, coslat1;
	float lat2, lon2, coslat2;
	int i;
	struct history_cell_t *history;

	if (!(pb->flags & F_HASPOS)) /* packet with a position.. (msgs with RECEIVER's position) */
		return 0;
	
	if (c->loc_known) {
		/* If client has sent a location, use it. The client may be an unvalidated
		 * client which does not exist in the historydb.
		 */
		r = maidenhead_km_distance(c->lat, c->cos_lat, c->lng, pb->lat, pb->cos_lat, pb->lng);
		if (r < f->h.f_dist)  /* Range is less than given limit */
			return f->h.negation ? 2 : 1;
		
		return 0;
	}
	
	/* Otherwise, fall back to looking up from the historydb */
	 
	if (!*c->username) /* Should not happen... */
		return 0;
	
	if (f->h.hist_age < tick || f->h.hist_age > tick + HIST_LOOKUP_INTERVAL) {
		i = historydb_lookup( c->username, strlen(c->username), &history );
		f->h.numnames = i;
		if (!i) {
			f->h.hist_age = tick + HIST_LOOKUP_INTERVAL/2;
			return 0; /* no result */
		}
		f->h.hist_age = tick + HIST_LOOKUP_INTERVAL;
		f->h.f_latN   = history->lat;
		f->h.f_lonE   = history->lon;
		f->h.f_coslat = history->coslat;
	}
	
	if (!f->h.numnames)
		return 0; /* cached lookup invalid.. */
	
	lat1    = f->h.f_latN;
	lon1    = f->h.f_lonE;
	coslat1 = f->h.f_coslat;
	
	lat2    = pb->lat;
	lon2    = pb->lng;
	coslat2 = pb->cos_lat;
	
	r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);
	if (r < f->h.f_dist)  /* Range is less than given limit */
		return f->h.negation ? 2 : 1;
	
	return 0;
}

static int filter_process_one_o(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* o/obj1/obj2...  	Object filter
	   Pass all objects with the exact name of obj1, obj2, ...
	   (* wild card allowed)
	   PROBABLY ALSO ITEMs

	   Usage frequency: 0.2%

	   .. 2 cases in entire APRS-IS core at any time.
	   About 50-70 invocations per second at peak.
	*/
	struct filter_refcallsign_t ref;
	int i;

	if ( (pb->packettype & (T_OBJECT|T_ITEM)) == 0 ) /* not an Object NOR Item */
		return 0;

	/* parse_aprs() has picked item/object name pointer and length.. */
	i = pb->srcname_len;
	if (i < 1 || i > CALLSIGNLEN_MAX) return 0; /* Bad object/item name */

	/* object name */
	memcpy( ref.callsign, pb->srcname, i);
	memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

	return filter_match_on_callsignset(&ref, i, f, MatchWild);
}

static int filter_process_one_p(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{

	/* p/aa/bb/cc...  	Prefix filter
	   Pass traffic with fromCall that start with aa or bb or cc...

	   Usage frequency: 14.4%

	   .. 80-100 cases in entire APRS-IS core at any time.
	   Up to 3500 invocations per second at peak.
	*/

	struct filter_refcallsign_t ref;
	int i = pb->srccall_end - pb->data;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	/* source address  "addr">... */
	memcpy( ref.callsign, pb->data, i);
	memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

	int r = filter_match_on_callsignset(&ref, i, f, MatchPrefix);
	
	/* match 3rd-party packets based on their innermost srccall,
	 * only works on non-object/item currently
	 */
	if (r == 0 && pb->srcname != pb->data && (pb->packettype & (T_OBJECT|T_ITEM)) == 0) {
		i = pb->srcname_len;

		if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

		memcpy( ref.callsign, pb->srcname, i);
		memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

		return filter_match_on_callsignset(&ref, i, f, MatchPrefix);
	}
	
	return r;
}

static int filter_process_one_q(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* q/con/ana  	q Contruct filter

	   q = q Construct command
	   con = list of q Construct to pass (case sensitive)
	   ana = analysis based on q Construct.

	   I = Pass positions from IGATES identified by qAr or qAR.

	   For example:
	   q/C    Pass all traffic with qAC
	   q/rR   Pass all traffic with qAr or qAR
	   q//I   Pass all position packets from IGATES identified
	          in other packets by qAr or qAR

	   Usage frequency: 0.4%

	   .. 2-6 cases in entire APRS-IS core at any time.
	   Up to 200 invocations per second at peak.
	*/

	const char *e = pb->qconst_start+2;
	int mask;

	switch (*e) {
	case 'C':
		mask = QC_C;
		break;
	case 'X':
		mask = QC_X;
		break;
	case 'U':
		mask = QC_U;
		break;
	case 'o':
		mask = QC_o;
		break;
	case 'O':
		mask = QC_O;
		break;
	case 'S':
		mask = QC_S;
		break;
	case 'r':
		mask = QC_r;
		break;
	case 'R':
		mask = QC_R;
		break;
	case 'Z':
		mask = QC_Z;
		break;
	case 'I':
		mask = QC_I;
		break;
	default:
		return 0; /* Should not happen... */
		break;
	}

	if (f->h.bitflags & mask) {
		/* Something matched! */
		return 1;
	}
	if (f->h.bitflags & QC_AnalyticsI) {
		/* Oh ?  Analytical! 
		   Has it ever been accepted into entry-igate database ? */
		if (filter_entrycall_lookup(pb))
			return 1; /* Found on entry-igate database! */
	}

	return 0; /* No match */
}


static int filter_process_one_r(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* r/lat/lon/dist  	Range filter

	   Pass posits and objects within dist km from lat/lon.
	   lat and lon are signed degrees, i.e. negative for West/South
	   and positive for East/North.

	   Multiple range filters can be defined at the same time.

	   Messages addressed to stations within the range are also passed.
	   (by means of aprs packet parse finding out the location..)

	   About 120-150 r-filters in entire APRS-IS core at any given time.
	   Up to 5200 invocations per second at peak.
	*/

	float lat1    = f->h.f_latN;
	float lon1    = f->h.f_lonE;
	float coslat1 = f->h.f_coslat;
	float r;

	float lat2, lon2, coslat2;

	if (!(pb->flags & F_HASPOS)) /* packet with a position.. (msgs with RECEIVER's position) */
		return 0;

	lat2    = pb->lat;
	lon2    = pb->lng;
	coslat2 = pb->cos_lat;

	r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);

	// hlog(LOG_DEBUG, "r: lalo: %.4f %.4f / reflalo: %.4f %.4f / dist: %.2f", rad2deg(lat2),rad2deg(lon2), rad2deg(lat1), rad2deg(lon1), r);

	if (r < f->h.f_dist)  /* Range is less than given limit */
		return (f->h.negation) ? 2 : 1;

	return 0;
}

static int filter_process_one_s(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* s/pri/alt/over  	Symbol filter

	   pri = symbols in primary table
	   alt = symbols in alternate table
	   over = overlay character (case sensitive)

	   For example:
	   s/->   This will pass all House and Car symbols (primary table)
	   s//#   This will pass all Digi with or without overlay
	   s//#/T This will pass all Digi with overlay of capital T

	   About 10-15 s-filters in entire APRS-IS core at any given time.
	   Up to 520 invocations per second at peak.
	*/
	const char symtable = (pb->symbol[0] == '/') ? '/' : '\\';
	const char symcode  = pb->symbol[1];
	const char symolay  = (pb->symbol[0] != symtable) ? pb->symbol[0] : 0;

	// hlog( LOG_DEBUG, "s-filt %c|%c|%c  %s", symtable, symcode, symolay ? symolay : '-', f->h.text );

	if (f->h.len1 != 0) {
		/* Primary table symbols */
		if ( symtable == '/' &&
		     memchr(f->h.text+f->h.len1s, symcode, f->h.len1) != NULL )
			return f->h.negation ? 2 : 1;
		// return 0;
	}
	if (f->h.lens.len3 != 0) {
		/* Secondary table with overlay */
		if ( memchr(f->h.text+f->h.lens.len3s, symolay, f->h.lens.len3) == NULL )
			return 0; // No match on overlay
		if ( memchr(f->h.text+f->h.lens.len2s, symcode, f->h.lens.len2) == NULL )
			return 0; // No match on overlay
		return f->h.negation ? 2 : 1;
	}
	/* OK, no overlay... */
	if (f->h.lens.len2 != 0) {
		/* Secondary table symbols */
		if ( symtable == '\\' &&
		     memchr(f->h.text+f->h.lens.len2s, symcode, f->h.lens.len2) != NULL )
			return f->h.negation ? 2 : 1;
	}
	/* No match */
	return 0;
}

static int filter_process_one_t(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* [-]t/poimntqsu
	   [-]t/poimntqsu/call/km

	   Type filter 	Pass all traffic based on packet type.
	   One or more types can be defined at the same time, t/otq
	   is a valid definition.

	   c = CWOP (local extension)
	   * = ALL  (local extension)

	   i = Items
	   m = Message
	   n = NWS Weather & Weather Objects
	   o = Objects
	   p = Position packets
	   q = Query
	   s = Status
	   t = Telemetry
	   u = User-defined
	   w = Weather

	   Note: The weather type filter also passes positions packets
	   for positionless weather packets.
	       
	   The second format allows putting a radius limit around "call"
	   (station callsign-SSID or object name) for the requested station
	   types.

	   About 40-60 s-filters in entire APRS-IS core at any given time.
	   Up to 2100 invocations per second at peak.

	   For the second format perhaps 2-3 in APRS-IS at any time.
	   (mapping to 60-100 invocations per second)

	   Usage examples:

	   -t/c              Everything except CWOP
	    t/.*./OH2RDY/50  Everything within 50 km of OH2RDY's last known position
	                     ("." is dummy addition for C comments..)
	*/
	int rc = 0;
	if (pb->packettype & f->h.bitflags) /* bitflags as comparison bitmask */
		rc = 1;

	if (!rc && (f->h.bitflags & T_WX) && (pb->flags & F_HASPOS)) {
		/* "Note: The weather type filter also passes positions packets
		//        for positionless weather packets."
		//
		// 1) recognize positionless weather packets
		// 2) register their source callsigns, do this in  input_parse
		// 3) when filtering for weather data, check non-weather 
		//    recognized packets against the database in point 2
		// 4) pass on packets matching point 3
		*/

		rc = filter_wx_lookup(pb);
	}
	/* Either it stops here, or it continues... */

	if (rc && f->h.type == 'T') { /* Within a range of callsign ?
				       * Rather rare..  perhaps 2-3 in APRS-IS.
				       */
		const char *callsign    = f->h.refcallsign.callsign;
		const int   callsignlen = f->h.refcallsign.reflen;
		float range, r;
		float lat1, lon1, coslat1;
		float lat2, lon2, coslat2;
		struct history_cell_t *history;
		int i;

		/* hlog(LOG_DEBUG, "Type filter with callsign range used! '%s'", f->h.text); */

		if (!(pb->flags & F_HASPOS)) /* packet with a position.. (msgs with RECEIVER's position) */
			return 0; /* No positional data.. */

		range = f->h.f_dist;

		/* So..  Now we have a callsign, and we have range.
		   Lets find callsign's location, and range to that item..
		   .. 60-100 lookups per second. */

		if (f->h.hist_age < tick || f->h.hist_age > tick + HIST_LOOKUP_INTERVAL) {
			i = historydb_lookup( callsign, callsignlen, &history );
			f->h.numnames = i;

			/* hlog( LOG_DEBUG, "Type filter with callsign range used! call='%s', range=%.1f position %sfound",
			//       callsign, range, i ? "" : "not ");
			*/


			if (!i) return 0; /* no lookup result.. */
			f->h.hist_age = tick + HIST_LOOKUP_INTERVAL;
			f->h.f_latN   = history->lat;
			f->h.f_lonE   = history->lon;
			f->h.f_coslat = history->coslat;
		}
		if (!f->h.numnames) return 0; /* No valid data at range center position cache */

		lat1    = f->h.f_latN;
		lon1    = f->h.f_lonE;
		coslat1 = f->h.f_coslat;

		lat2    = pb->lat;
		lon2    = pb->lng;
		coslat2 = pb->cos_lat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);

		if (r < range)  /* Range is less than given limit */
			return (f->h.negation) ? 2 : 1;

		return 0; /* unimplemented! */
	}

	return (f->h.negation ? (rc+rc) : rc);
}

static int filter_process_one_u(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* u/unproto1/unproto2/...  	Unproto filter

	   This filter passes all packets with the specified destination
	   callsign-SSID(s) (also known as the To call or unproto call).
	   Supports * wild card.

	   Seen hardly ever in APRS-IS core, some rare instances in Tier-2.
	*/

	struct filter_refcallsign_t ref;
	const char *d = pb->srccall_end+1;
	int i;

	i = pb->dstcall_len;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	/* hlog( LOG_INFO, "unproto:  '%.*s' -> '%.*s'",
	//       (int)(pb->packet_len < 30 ? pb->packet_len : 30), pb->data, (int)i, d);
	*/

	/* destination address  ">addr," */
	memcpy( ref.callsign,   d, i);
	memset( ref.callsign+i, 0, sizeof(ref.callsign)-i );

	return filter_match_on_callsignset(&ref, i, f, MatchWild);
}

static int filter_process_one(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	int rc = 0;

	switch (f->h.type) {

	case 'a':
	case 'A':
		rc = filter_process_one_a(c, pb, f);
		break;

	case 'b':
	case 'B':
		rc = filter_process_one_b(c, pb, f);
		break;

	case 'd':
	case 'D':
		rc = filter_process_one_d(c, pb, f);
		break;

	case 'e':
	case 'E':
		rc = filter_process_one_e(c, pb, f);
		break;

	case 'f':
	case 'F':
		rc = filter_process_one_f(c, pb, f);
		break;

	case 'g':
	case 'G':
		rc = filter_process_one_g(c, pb, f);
		break;

	case 'm':
	case 'M':
		rc = filter_process_one_m(c, pb, f);
		break;

	case 'o':
	case 'O':
		rc = filter_process_one_o(c, pb, f);
		break;

	case 'p':
	case 'P':
		rc = filter_process_one_p(c, pb, f);
		break;

	case 'q':
	case 'Q':
		rc = filter_process_one_q(c, pb, f);
		break;

	case 'r':
	case 'R':
		rc = filter_process_one_r(c, pb, f);
		break;

	case 's':
	case 'S':
		rc = filter_process_one_s(c, pb, f);
		break;

	case 't':
	case 'T':
		rc = filter_process_one_t(c, pb, f);
		break;

	case 'u':
	case 'U':
		rc = filter_process_one_u(c, pb, f);
		break;

	default:
		rc = -1;
		break;
	}
	//hlog(LOG_DEBUG, "filter '%s'  rc=%d", f->h.text, rc);

	return rc;
}

int filter_process(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	struct filter_t *f;
	
	/* messaging support: if (1) this is a text message,
	 * (2) the client is an igate port,
	 * and (3) the message's recipient has been heard
	 * recently on the port, gate the message.
	 */
	if (c->flags & CLFLAGS_IGATE) {
		if (pb->packettype & T_MESSAGE) {
			if (
				(pb->dstname_len == c->username_len && memcmp(pb->dstname, c->username, c->username_len) == 0)
				|| (client_heard_check(c, pb->dstname, pb->dstname_len, pb->dstname_hash))
			) {
				/* insert the source callsign to the courtesy position list */
				client_courtesy_update(c, pb);
				return 1;
			}
		}
		/* Courtesy position: if a message from this source callsign has been
		 * passed to this socket within 30 minutes, do pass on the next
		 * single position packet, too.
		 */
		if (pb->packettype & (T_POSITION|T_OBJECT|T_ITEM)
			&& client_courtesy_needed(c, pb)) {
				FILTER_CLIENT_DEBUG(self, c, "# courtesy position after message\r\n", NULL);
				return 1;
		}
		/* If the source callsign of a packet having TCPIP* in the path has been
		 * recently heard on this socket, do pass on the packets to this socket too.
		 * This lets igates know that the station is also available on the Internet,
		 * and no TX igating to RF should be done.
		 */
		if ((pb->flags & F_HAS_TCPIP) && client_heard_check(c, pb->data, pb->srccall_end - pb->data, pb->srccall_hash)) {
			FILTER_CLIENT_DEBUG(self, c, "# igate support TCPIP* packet from a heard station\r\n", NULL);
			return 1;
		}
	}
	
	f = c->negdefaultfilters;
	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		/* no reports to user about bad filters.. */
		if (rc > 0)
			return 0; // match on filter - no output on client
	}

	f = c->posdefaultfilters;
	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		/* no reports to user about bad filters.. */
		if (rc > 0) {
			FILTER_CLIENT_DEBUG(self, c, "# matched server default filter %s\r\n", f->h.text);
			return rc;
		}
	}

	f = c->neguserfilters;
	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		if (rc < 0) {
			rc = client_bad_filter_notify(self, c, f->h.text);
			if (rc < 0) /* possibly the client got destroyed here! */
				return rc;
		}
		if (rc > 0) {
			FILTER_CLIENT_DEBUG(self, c, "# matched negative filter %s\r\n", f->h.text);
			return 0; // match on filter - no output on client
		}
	}

	f = c->posuserfilters;
	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		if (rc < 0) {
			rc = client_bad_filter_notify(self, c, f->h.text);
			if (rc < 0) /* possibly the client got destroyed here! */
				return rc;
		}
		if (rc > 0) {
			FILTER_CLIENT_DEBUG(self, c, "# matched filter %s\r\n", f->h.text);
			return rc;
		}
	}
	
	return 0;
}

/*
 *	Send a reply to a filter command, either using a message or through a comment
 *	line on the IS stream
 */

static int filter_command_reply(struct worker_t *self, struct client_t *c, int in_message, const char *fmt, ...)
{
	va_list args;
	char s[PACKETLEN_MAX];
	
	va_start(args, fmt);
	vsnprintf(s, PACKETLEN_MAX, fmt, args);
	va_end(args);
	
	if (!in_message)
		return client_printf(self, c, "# %s\r\n", s);
	
	return messaging_message_client(self, c, "%s", s);
}

/*
 *	filter_commands() implements treatment for incoming client filter requests.
 *
 *	Return value propagates negative returns from things like  client_write()
 *	indicating the struct client_t * object being destroyed.
 */
int filter_commands(struct worker_t *self, struct client_t *c, int in_message, const char *s, int len)
{
	char *argv[256];
	struct filter_t *f;
	char *b, *p;
	int i, argc;

	/* skip over the #filter in the beginning of the command */
	len -= 6;
	s   += 6;

	if ( *s == '?' && len == 1 ) {
		/* Query current filters */
		int lensum = 0;
		
		for (f = c->neguserfilters; f; f = f->h.next) {
			lensum += 2 + strlen(f->h.text);
		}
		for (f = c->posuserfilters; f; f = f->h.next) {
			lensum += 2 + strlen(f->h.text);
		}

		p = b = alloca(lensum+20);
		p += sprintf(b, "# filters: ");
		for (f = c->neguserfilters; f; f = f->h.next) {
			p += sprintf(p, "%s ", f->h.text);
		}
		for (f = c->posuserfilters; f; f = f->h.next) {
			p += sprintf(p, "%s ", f->h.text);
		}
		p += sprintf(p, "\r\n");
		/* client can be destroyed here.. */
		return c->write(self, c, b, (int)(p-b));
	}
	if (*s != ' ') {
		return filter_command_reply(self, c, in_message, "Filter: Bad input, no space character after filter command");
	}
	++s;
	--len;

	if ( strncasecmp(s, "default", 7) == 0 && len == 7 ) {
		/* discard any user defined filters that possibly were
		   injected on this connection.  */
		f = c->neguserfilters;
		filter_free(f);
		c->neguserfilters = NULL;

		f = c->posuserfilters;
		filter_free(f);
		c->posuserfilters = NULL;
		// FIXME: Sleep a bit ? ... no, that would be a way to create a denial of service attack
		// FIXME: there is a danger of SEGV-blowing filter processing...
		return filter_command_reply(self, c, in_message, "User filters reset to default");
	}

	/* new filter definitions to supersede previous ones */

	/* Discard old ones. */
	f = c->neguserfilters;
	filter_free(f);
	c->neguserfilters = NULL;

	f = c->posuserfilters;
	c->posuserfilters = NULL;
	filter_free(f);
	// FIXME: Sleep a bit ? ... no, that would be a way to create a denial of service attack
	// FIXME: there is a danger of SEGV-blowing filter processing...

	b = hmalloc(len+2);
	memcpy(b, s, len);
	b[len] = 0;
	
	// archive a copy of the filters, for status display
	strncpy(c->filter_s, b, FILTER_S_SIZE);
	c->filter_s[FILTER_S_SIZE-1] = 0;
	sanitize_ascii_string(c->filter_s);
	
	argc = parse_args( argv, b );
	for (i = 0; i < argc; ++i) {
		filter_parse(c, argv[i], 1); /* user filters */
	}
	hfree(b);
	
	return filter_command_reply(self, c, in_message, "filter %s active", c->filter_s);
}

/*
 *	cellmalloc status
 */

#ifndef _FOR_VALGRIND_
void filter_cell_stats(struct cellstatus_t *filter_cellst,
	struct cellstatus_t *filter_entrycall_cellst,
	struct cellstatus_t *filter_wx_cellst)
{
	cellstatus(filter_cells, filter_cellst);
	cellstatus(filter_entrycall_cells, filter_entrycall_cellst);
	cellstatus(filter_wx_cells, filter_wx_cellst);
}
#endif
