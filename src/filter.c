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

// FIXME: filters: d e q s

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>

#include <math.h>

#include "hmalloc.h"
#include "hlog.h"
#include "worker.h"
#include "filter.h"
#include "cellmalloc.h"
#include "historydb.h"

/*
  See:  http://www.aprs-is.net/javaprssrvr/javaprsfilter.htm

  a/latN/lonW/latS/lonE Area filter
  b/call1/call2...  	Budlist filter (*)
  d/digi1/digi2...  	Digipeater filter (*)
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

  Sample usage frequencies:

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


// FIXME:  What exactly is the meaning of negation on the pattern ?
//         Match as a failure to match, and stop searching ?
//         Something filter dependent ?

#define WildCard      0x80  // it is wild-carded prefix string
#define NegationFlag  0x40  // 
#define LengthMask    0x0F  // only low 4 bits encode length

// values above are chosen for 4 byte alignment..

struct filter_refcallsign_t {
	union {
		char	  callsign[CALLSIGNLEN_MAX+1]; // size: 10..
		int32_t	  cc[2]; // makes alignment!   // size:  8
	}; // ANONYMOUS UNION
	uint8_t	reflen; // length and flags
};
struct filter_head_t {
	struct filter_t *next;
	const char *text; /* filter text as is		*/
	float   f_latN, f_lonE, f_latS, f_lonW;
			/* parsed floats, if any	*/
#define f_dist   f_latS /* for R filter */
#define f_coslat f_lonW /* for R filter */

	char	type;	  /* 1 char			*/
	uint8_t	negation; /* boolean flag		*/
	int 	numnames;
	union {
		struct filter_refcallsign_t  refcallsign;   // for cases where there is only one..
		struct filter_refcallsign_t *refcallsigns;  // hmalloc()ed array, alignment important!
	};
};

struct filter_t {
	struct filter_head_t h;
#define FILT_TEXTBUFSIZE (128-sizeof(struct filter_head_t))
	char textbuf[FILT_TEXTBUFSIZE];
};

typedef enum {
	MatchExact,
	MatchPrefix,
	MatchWild
} MatchEnum;

#ifndef _FOR_VALGRIND_
cellarena_t *filter_cells;
#endif


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
#ifndef _FOR_VALGRIND_
	filter_cells = cellinit( sizeof(struct filter_t), __alignof__(struct filter_t),
				 CELLMALLOC_POLICY_LIFO, 128 /* 128 kB at the time */,
				 0 /* minfree */ );

	/* printf("filter: sizeof=%d alignof=%d\n",sizeof(struct filter_t),__alignof__(struct filter_t)); */
#endif
}

/*
 *	filter_match_on_callsignset()  matches prefixes, or exact keys
 *	on filters of types:  b, d, e, o, p, u  ('p' and 'b' need OPTIMIZATION)
 *
 */

static int filter_match_on_callsignset(struct filter_refcallsign_t *ref, int keylen, struct filter_t *f, MatchEnum wildok)
{
	int i;
	struct filter_refcallsign_t *r = f->h.refcallsigns;

	for (i = 0; i < f->h.numnames; ++i) {
		const int reflen = r[i].reflen;
		const int len    = reflen & LengthMask;
		const uint32_t *r1 = (const void*)ref->callsign;
		const uint32_t *r2 = (const void*)r[i].callsign;

		switch (wildok) {
		case MatchExact:
			if (len != keylen)
				continue; // no match
			// length OK, compare content - both buffers zero filled,
			// and size is constant -- let compiler do smarts with
			// constant comparison lengths...
			switch (len) {
			case 1: case 2: case 3: case 4:
			  if (memcmp( r1, r2, 4 ) != 0) continue;
			  break;
			case 5: case 6: case 7: case 8:
			  if (memcmp( r1, r2, 8 ) != 0) continue;
			  break;
			case 9:
			  if (memcmp( r1, r2, 9 ) != 0) continue;
			  break;
			default:
			  return -1;
			  break;
			}
			// So it was an exact match
			// Precisely speaking..  we should check that there is
			// no WildCard flag, or such.  But then this match
			// method should not be used if parser finds any such.
			return ( reflen & NegationFlag ? 2 : 1 );
			break;
		case MatchPrefix:
			if (len > keylen || !len) {
				// reference string length is longer than our key
				continue;
			}
			// Let compiler do smarts - it "knowns" alignment, and
			// the length is constant...
			switch (len) {
			case 1:
			  if (memcmp( r1, r2, 1 ) != 0) continue;
			  break;
			case 2:
			  if (memcmp( r1, r2, 2 ) != 0) continue;
			  break;
			case 3:
			  if (memcmp( r1, r2, 3 ) != 0) continue;
			  break;
			case 4:
			  if (memcmp( r1, r2, 4 ) != 0) continue;
			  break;
			case 5:
			  if (memcmp( r1, r2, 5 ) != 0) continue;
			  break;
			case 6:
			  if (memcmp( r1, r2, 6 ) != 0) continue;
			  break;
			case 7:
			  if (memcmp( r1, r2, 7 ) != 0) continue;
			  break;
			case 8:
			  if (memcmp( r1, r2, 8 ) != 0) continue;
			  break;
			case 9:
			  if (memcmp( r1, r2, 9 ) != 0) continue;
			  break;
			default:
			  return -1;
			  break;
			}

			return ( reflen & NegationFlag ? 2 : 1 );
			break;
		case MatchWild:
			if (len > keylen || !len) {
				// reference string length is longer than our key
				continue;
			}
			// Let compiler do smarts - it "knowns" alignment, and
			// the length is constant...
			switch (len) {
			case 1:
			  if (memcmp( r1, r2, 1 ) != 0) continue;
			  break;
			case 2:
			  if (memcmp( r1, r2, 2 ) != 0) continue;
			  break;
			case 3:
			  if (memcmp( r1, r2, 3 ) != 0) continue;
			  break;
			case 4:
			  if (memcmp( r1, r2, 4 ) != 0) continue;
			  break;
			case 5:
			  if (memcmp( r1, r2, 5 ) != 0) continue;
			  break;
			case 6:
			  if (memcmp( r1, r2, 6 ) != 0) continue;
			  break;
			case 7:
			  if (memcmp( r1, r2, 7 ) != 0) continue;
			  break;
			case 8:
			  if (memcmp( r1, r2, 8 ) != 0) continue;
			  break;
			case 9:
			  if (memcmp( r1, r2, 9 ) != 0) continue;
			  break;
			default:
			  return -1;
			  break;
			}

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
 *	If previous filter was of same type as this one, that one's refbuf is extended.
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
	// count the number of prefixes in there..
	while (*p) {
		if (*p) ++refmax;
		while (*p && *p != '/') ++p;
		if (*p == '/') ++p;
	}
	if (refmax == 0) return -1; // No prefixes ??

	if (ff && ff->h.type == f0->h.type) { // SAME TYPE, extend previous record!
		extend = 1;
		refcount = ff->h.numnames + refmax;
		refbuf   = hrealloc(ff->h.refcallsigns, sizeof(*refbuf) * refcount);
		ff->h.refcallsigns = refbuf;
		refcount = ff->h.numnames;
	} else {
		refbuf = hmalloc(sizeof(*refbuf)*refmax);
		refcount = 0;
	}

	p = filt0;
	if (*p == '-') ++p;
	while (*p && *p != '/') ++p;
	if (*p == '/') ++p;

	// hlog(LOG_DEBUG, "p-filter: '%s' vs. '%s'", p, keybuf);
	while (*p)  {
		k = prefixbuf;
		memset(prefixbuf, 0, sizeof(prefixbuf));
		i = 0;
		wildcard = 0;
		while (*p != 0 && *p != '/' && i < (CALLSIGNLEN_MAX)) {
			if (*p == '*') {
				wildcard = 1;
				if (wildok != MatchWild)
					return -1;
				continue;
			}
			*k = *p;
			++p;
			++k;
		}
		*k = 0;
		/* OK, we have one prefix part collected, scan source until next '/' */
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

	f0->h.refcallsigns = refbuf;
	f0->h.numnames     = refcount;
	if (extend) {
		char *s;
		ff->h.numnames     = refcount;
		i = strlen(ff->h.text) + strlen(filt0)+2;
		if (i <= FILT_TEXTBUFSIZE) {
			// Fits in our built-in buffer block - like previous..
			// Append on existing buffer
			s = ff->textbuf + strlen(ff->textbuf);
			sprintf(s, " %s", filt0);
		} else {
			// It does not fit anymore..
			s = hmalloc(i); // alloc a new one
			sprintf(s, "%s %s", p, filt0); // .. and catenate.
			p = ff->h.text;
			if (ff->h.text != ff->textbuf) // possibly free old
				hfree((void*)p);
			ff->h.text = s;     // store new
		}
	}
	/* If not extending existing filter item, let main parser do the finalizations */

	return extend;
}


int filter_parse(struct client_t *c, const char *filt, int is_user_filter)
{
	struct filter_t *f, f0;
	int i;
	const char *filt0 = filt;
	const char *s;
	char dummyc;
	struct filter_t *ff, **ffp;

	if (is_user_filter)
		ffp = &c->userfilters;
	else
		ffp = &c->defaultfilters;

	ff = *ffp;
	for ( ; ff && ff->h.next; ff = ff->h.next)
	  ;
	/* ff  points to last so far accumulated filter,
	   if none were previously received, it is NULL.. */

	memset(&f0, 0, sizeof(f0));
	if (*filt == '-') {
		f0.h.negation = 1;
		++filt;
	}
	f0.h.type = *filt;

	if (!strchr("abdeopqrstuABDEOPQRSTU",*filt)) {
		/* Not valid filter code */
		hlog(LOG_DEBUG, "Bad filter: %s", filt0);
		return -1;
	}

	switch (f0.h.type) {
	case 'a':
	case 'A':
		/*  a/latN/lonW/latS/lonE     Area filter -- OPTIMIZE!  */

		i = sscanf(filt, "a/%f/%f/%f/%f%c",
			   &f0.h.f_latN, &f0.h.f_lonW,
			   &f0.h.f_latS, &f0.h.f_lonE, &dummyc);

		if (i != 4) {
			hlog(LOG_DEBUG, "Bad parse: %s", filt0);
			return -1;
		}

		if (!( -90.01 < f0.h.f_latN && f0.h.f_latN <  90.01)) {
			hlog(LOG_DEBUG, "Bad latN value: %s", filt0);
			return -2;
		}
		if (!(-180.01 < f0.h.f_lonW && f0.h.f_lonW < 180.01)) {
			hlog(LOG_DEBUG, "Bad lonW value: %s", filt0);
			return -2;
		}
		if (!( -90.01 < f0.h.f_latS && f0.h.f_latS <  90.01)) {
			hlog(LOG_DEBUG, "Bad latS value: %s", filt0);
			return -2;
		}
		if (!(-180.01 < f0.h.f_lonE && f0.h.f_lonE < 180.01)) {
			hlog(LOG_DEBUG, "Bad lonE value: %s", filt0);
			return -2;
		}

		if (f0.h.f_latN < f0.h.f_latS) {
			hlog(LOG_DEBUG, "Bad: latN<latS: %s", filt0);
			return -3; /* expect: latN >= latS */
		}
		if (f0.h.f_lonW > f0.h.f_lonE) {
			hlog(LOG_DEBUG, "Bad: lonW>lonE: %s", filt0);
			return -3; /* expect: lonW <= lonE */
		}

		hlog(LOG_DEBUG, "Filter: %s -> A %.3f %.3f %.3f %.3f", filt0, f0.h.f_latN, f0.h.f_lonW, f0.h.f_latS, f0.h.f_lonE);
		
		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonW = filter_lon2rad(f0.h.f_lonW);
		
		f0.h.f_latS = filter_lat2rad(f0.h.f_latS);
		f0.h.f_lonE = filter_lon2rad(f0.h.f_lonE);

		break;

	case 'b':
	case 'B':
		/*  b/call1/call2...   Budlist filter (*) */

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, ffp, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) // extended previous
			return 0;


		break;

	case 'd':
	case 'D':
		/* d/digi1/digi2...  	Digipeater filter (*)	*/

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, ffp, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) // extended previous
			return 0;

		break;

	case 'e':
	case 'E':
		/*   e/call1/call1/...  Entry station filter (*) */

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, ffp, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) // extended previous
			return 0;

		break;

	case 'f':
	case 'F':
		/*  f/call/dist         Friend's range filter  */

		i = sscanf(filt, "r/%9[^/]/%f", f0.h.refcallsign.callsign, &f0.h.f_dist);
		if (i != 2 || f0.h.f_dist < 0.1) {
			hlog(LOG_DEBUG, "Bad parse: %s", filt0);
			return -1;
		}

		f0.h.refcallsign.callsign[CALLSIGNLEN_MAX] = 0;
		f0.h.refcallsign.reflen = strlen(f0.h.refcallsign.callsign);
		f0.h.numnames = 1;

		hlog(LOG_DEBUG, "Filter: %s -> F xxx %.3f", filt0, f0.h.f_dist);

		// NOTE: Could do static location resolving at connect time, 
		// and then use the same way as 'r' range does.  The friends
		// are rarely moving...

		break;

	case 'm':
	case 'M':
		/*  m/dist            My range filter  */

		i = sscanf(filt, "r/%f", &f0.h.f_dist);
		if (i != 1 || f0.h.f_dist < 0.1) {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}

		hlog(LOG_DEBUG, "Filter: %s -> M %.3f", filt0, f0.h.f_dist);

		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonW = filter_lon2rad(f0.h.f_lonW);

		f0.h.f_coslat = cosf( f0.h.f_latN ); /* Store pre-calculated COS of LAT */
		break;

	case 'o':
	case 'O':
		/* o/obje1/obj2...  	Object filter (*)	*/

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, ffp, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) // extended previous
			return 0;

		break;

	case 'p':
	case 'P':
		/* p/aa/bb/cc...  	Prefix filter
		   Pass traffic with fromCall that start with aa or bb or cc...
		*/
		i = filter_parse_one_callsignset(c, filt0, &f0, ff, ffp, MatchPrefix );
		if (i < 0)
			return i;
		if (i > 0) // extended previous
			return 0;

		break;

	case 'r':
	case 'R':
		/*  r/lat/lon/dist            Range filter  */

		i = sscanf(filt, "r/%f/%f/%f",
			 &f0.h.f_latN, &f0.h.f_lonW, &f0.h.f_dist);
		if (i != 3 || f0.h.f_dist < 0.1) {
			hlog(LOG_DEBUG, "Bad parse: %s", filt0);
			return -1;
		}

		if (!( -90.01 < f0.h.f_latN && f0.h.f_latN <  90.01)) {
			hlog(LOG_DEBUG, "Bad lat value: %s", filt0);
			return -2;
		}
		if (!(-180.01 < f0.h.f_lonW && f0.h.f_lonW < 180.01)) {
			hlog(LOG_DEBUG, "Bad lon value: %s", filt0);
			return -2;
		}

		hlog(LOG_DEBUG, "Filter: %s -> R %.3f %.3f %.3f", filt0, f0.h.f_latN, f0.h.f_lonW, f0.h.f_dist);

		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonW = filter_lon2rad(f0.h.f_lonW);

		f0.h.f_coslat = cosf( f0.h.f_latN ); /* Store pre-calculated COS of LAT */
		break;

	case 's':
	case 'S':
		/* s/pri/alt/over  	Symbol filter  */
	  // FIXME: S-filter pre-parser
		break;

	case 't':
	case 'T':
		/* t/..............
		   t/............../call/km
		*/
		s = filt+1;
		f0.h.type = 't';
		// re-use  f0.h.numnames  field for T_** flags
		f0.h.numnames = 0;

		if (*s++ != '/') {
			hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
			return -1;
		}
		for ( ; *s && *s != '/'; ++s ) {
			switch (*s) {
			case '*':
				f0.h.numnames |= 0x0FFF; // "ALL"
				break;
			case 'c': case 'C':
				f0.h.numnames |= T_CWOP;
				break;
			case 'i': case 'I':
				f0.h.numnames |= T_ITEM;
				break;
			case 'm': case 'M':
				f0.h.numnames |= T_MESSAGE;
				break;
			case 'n': case 'N':
				f0.h.numnames |= T_NWS;
				break;
			case 'o': case 'O':
				f0.h.numnames |= T_OBJECT;
				break;
			case 'p': case 'P':
				f0.h.numnames |= T_POSITION;
				break;
			case 'q': case 'Q':
				f0.h.numnames |= T_QUERY;
				break;
			case 's': case 'S':
				f0.h.numnames |= T_STATUS;
				break;
			case 't': case 'T':
				f0.h.numnames |= T_TELEMETRY;
				break;
			case 'u': case 'U':
				f0.h.numnames |= T_USERDEF;
				break;
			default:
				hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
				return -1;
			}
		}
		if (*s == '/' && s[1] != 0) { /* second format */
			i = sscanf(s, "/%9[^/]/%f%c", f0.h.refcallsign.callsign, &f0.h.f_dist, &dummyc);
			if ( i != 2 || f0.h.f_dist < 0.1 || // 0.1 km minimum radius
			     strlen(f0.h.refcallsign.callsign) < CALLSIGNLEN_MIN ) {
				hlog(LOG_DEBUG, "Bad filter parse: %s", filt0);
				return -1;
			}
			f0.h.refcallsign.callsign[CALLSIGNLEN_MAX] = 0;
			f0.h.refcallsign.reflen = strlen(f0.h.refcallsign.callsign);
			f0.h.type = 'T'; // two variants...
		}

		break;

	case 'u':
	case 'U':
		/* u/unproto1/unproto2...  	Unproto filter (*)	*/

		i = filter_parse_one_callsignset(c, filt0, &f0, ff, ffp, MatchWild );
		if (i < 0)
			return i;
		if (i > 0) // extended previous
			return 0;

		break;



	default:;
		// No pre-parsers for other types
		hlog(LOG_DEBUG, "Filter: %s", filt0);
		break;
	}
	
	/* OK, pre-parsing produced accepted result */
#ifndef _FOR_VALGRIND_
	f = cellmalloc(filter_cells);
	if (!f) return -1;
	*f = f0; /* store pre-parsed values */
	if (strlen(filt) < FILT_TEXTBUFSIZE) {
		strcpy(f->textbuf, filt);
		f->h.text = f->textbuf;
	} else
		f->h.text = hstrdup(filt); /* and copy of filter text */
#else
	f = hmalloc(sizeof(*f) + strlen(filt));
	*f = f0; /* store pre-parsed values */
	f->h.text = f->textbuf;
	strcpy(f->textbuf, filt); /* and copy of filter text */
#endif

	// hlog(LOG_DEBUG, "parsed filter: t=%c n=%d '%s'", f->h.type, f->h.negation, f->h.text);

	/* link to the tail.. */
	if (ff)
		ffp = &ff->h.next;

	*ffp = f;

	return 0;
}

/* Discard the defined filter chain */
void filter_free(struct filter_t *f)
{
	struct filter_t *fnext;

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
	}
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

	   OPTIMIZE !
	*/
	;
	if (!(pb->flags & F_HASPOS)) // packet with a position.. (msgs with RECEIVER's position)
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

	   Optimize ? ("only" around 10% of usage cases)
	*/

	struct filter_refcallsign_t ref;
	int i = pb->srccall_end - pb->data;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// source address  "addr">...
	memset( &ref, 0, sizeof(ref) );
	memcpy( ref.callsign, pb->data, i);

	return filter_match_on_callsignset(&ref, i, f, MatchExact);
}

static int filter_process_one_d(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* d/digi1/digi2...  	Digipeater filter

	   The digipeater filter will pass all packets that have been
	   digipeated by a particular station(s) (the station's call
	   is in the path).   This filter allows the * wildcard.
	*/
	struct filter_refcallsign_t ref;
	int i;

	return -1; // FIXME: write d-filter

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// destination address  ">addr,"
	memset( &ref, 0, sizeof(ref) );
	memcpy( ref.callsign, pb->srccall_end+1, i);

	return filter_match_on_callsignset(&ref, i, f, MatchWild);
}

static int filter_process_one_e(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* e/call1/call1/...  	Entry station filter

	   This filter passes all packets with the specified
	   callsign-SSID(s) immediately following the q construct.
	   This allows filtering based on receiving IGate, etc.
	   Supports * wildcard.
	*/

	struct filter_refcallsign_t ref;
	int i;

	return -1; // FIXME: write e-filter

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// destination address  ">addr,"
	memset( &ref, 0, sizeof(ref) );
	memcpy( ref.callsign, pb->srccall_end+1, i);

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

	*/

	struct history_cell_t *history;

	float r;
	float lat1, lon1, coslat1;
	float lat2, lon2, coslat2;

	const char *callsign = f->h.refcallsign.callsign;
	int i = f->h.refcallsign.reflen;

	if (!(pb->flags & F_HASPOS)) // packet with a position.. (msgs with RECEIVER's position)
		return 0; // No position data...

	// find friend's last location packet
	i = historydb_lookup( callsign, i, &history );
	if (!i) return 0; // no lookup result..

	lat1    = history->lat;
	lon1    = history->lon;
	coslat1 = history->coslat;

	lat2    = pb->lat;
	lon2    = pb->lng;
	coslat2 = pb->cos_lat;

	r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);

	if (r < f->h.f_dist)  /* Range is less than given limit */
		return (f->h.negation) ? 2 : 1;

	return 0;
}

static int filter_process_one_m(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* m/dist  	My Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of the logged in client.

	   Messages addressed to stations within the range are also passed.
	   (by means of aprs packet parse finding out the location..)

	   OPTIMIZE! (21% of all filters!)

	   NOTE:  MY RANGE is rarely moving, once there is a positional
	   fix, it could stay fixed...    Or true historydb lookup frequency
	   could be limited to once per - say - every 100 seconds per any
	   given filter ?  (wants time_t variable into filter...)
	*/

	float lat1, lon1, coslat1;
	float lat2, lon2, coslat2;
	float r;
	int i;
	struct history_cell_t *history;


	if (!(pb->flags & F_HASPOS)) // packet with a position.. (msgs with RECEIVER's position)
		return 0;

	if (!c->username) // Should not happen...
		return 0;

	i = historydb_lookup( c->username, strlen(c->username), &history );
	if (!i) return 0; // no result

	lat1    = history->lat;
	lon1    = history->lon;
	coslat1 = history->coslat;


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
	*/
	struct filter_refcallsign_t ref;
	int i;

	const char *s;

	if ( (pb->packettype & (T_OBJECT|T_ITEM)) == 0 ) // not an Object NOR Item
		return 0;

	// Pick object name  ";item  *" or ";item  _" -- strip tail spaces
	// Pick item name    ")item!" or ")item_"     -- keep all spaces

	// FIXME?  These filters are very rare in real use...
	// FIXME: have parser to fill  pb->objlen  so this needs not to scan the buffer again.

	s = pb->info_start+1;
	if (pb->packettype & T_OBJECT) { // It is an Object - No embedded spaces!
		for (i = 0; i < CALLSIGNLEN_MAX; ++i, ++s) {
			if (*s == ' ' || *s == '*' || *s == '_')
				break;
		}
	} else { // It is an ITEM then..
		for (i = 0; i < CALLSIGNLEN_MAX; ++i, ++s) {
			if (*s == '!' || *s == '_') // Embedded space are OK!
				break;
		}
	}
	if (i < 1) return 0; /* Bad object/item name */

	// object name
	memcpy( ref.callsign, pb->info_start+1, i);
	memset( ref.callsign+i, 0, sizeof(ref)-i );

	return filter_match_on_callsignset(&ref, i, f, MatchWild);
}

static int filter_process_one_p(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{

	/* p/aa/bb/cc...  	Prefix filter
	   Pass traffic with fromCall that start with aa or bb or cc...

	   OPTIMIZE!
	*/

	struct filter_refcallsign_t ref;
	int i = pb->srccall_end - pb->data;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// source address  "addr">...
	memcpy( ref.callsign, pb->data, i);
	memset( ref.callsign+i, 0, sizeof(ref)-i );

	return filter_match_on_callsignset(&ref, i, f, MatchPrefix);
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
	*/

	// FIXME: write q-filter

	return -1;
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

	   OPTIMIZE!
	*/

	float lat1    = f->h.f_latN;
	float lon1    = f->h.f_lonE;
	float coslat1 = f->h.f_coslat;
	float r;

	float lat2, lon2, coslat2;

	if (!(pb->flags & F_HASPOS)) // packet with a position.. (msgs with RECEIVER's position)
		return 0;

	lat2    = pb->lat;
	lon2    = pb->lng;
	coslat2 = pb->cos_lat;

	r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);

	if (r < f->h.f_dist)  /* Range is less than given limit */
		return (f->h.negation) ? 2 : 1;

	return 0;
}

static int filter_process_one_s(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* s/pri/alt/over  	Symbol filter  	pri = symbols in primary table

	   alt = symbols in alternate table
	   over = overlay character (case sensitive)

	   For example:
	   s/->   This will pass all House and Car symbols (primary table)
	   s//#   This will pass all Digi with or without overlay
	   s//#/T This will pass all Digi with overlay of capital T
	*/

	// FIXME: write s-filter

	return -1;
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

	   p = Position packets
	   o = Objects
	   i = Items
	   m = Message
	   n = NWS Weather & Weather Objects
	   w = Weather
	   t = Telemetry
	   q = Query
	   s = Status
	   u = User-defined

	   Note: The weather type filter also passes positions packets
	   for positionless weather packets.
	       
	   The second format allows putting a radius limit around "call"
	   (station callsign-SSID or object name) for the requested station
	   types.

	   Usage examples:

	   -t/c              Everything except CWOP
	    t/.*./OH2RDY/50  Everything within 50 km of OH2RDY's last known position
	                     ("." is dummy addition for C comments..)
	*/
	int rc = 0;
	if (pb->packettype & f->h.numnames) // reused numnames as comparison bitmask
		rc = 1;

	/* Either it stops here, or it continues... */

	if (rc && f->h.type == 'T') { /* Within a range of callsign ? */
		const char *callsign    = f->h.refcallsign.callsign;
		const int   callsignlen = f->h.refcallsign.reflen;
		float range, r;
		float lat1, lon1, coslat1;
		float lat2, lon2, coslat2;
		struct history_cell_t *history;
		int i;

		hlog(LOG_DEBUG, "type filter with callsign range used! '%s'", f->h.text);

		if (!(pb->flags & F_HASPOS)) // packet with a position.. (msgs with RECEIVER's position)
			return 0; // No positional data..

		range = f->h.f_dist;

		/* So..  Now we have a callsign, and we have range.
		   Lets find callsign's location, and range to that item.. */

		i = historydb_lookup( callsign, callsignlen, &history );
		if (!i) return 0; // no lookup result..

		lat1    = history->lat;
		lon1    = history->lon;
		coslat1 = history->coslat;

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
	*/

	struct filter_refcallsign_t ref;
	int i;

	// dstcall_end DOES NOT initially include SSID!  Must scan it!
	// We can advance the destcall_end and leave it at the real end..
	while (pb->dstcall_end[0] != ',' && pb->dstcall_end[0] != ':')
	  pb->dstcall_end += 1;

	i = pb->dstcall_end - (pb->srccall_end+1); // *srccall_end == '>'

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// destination address  ">addr,"
	memset( &ref, 0, sizeof(ref) );
	memcpy( ref.callsign, pb->srccall_end+1, i);

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
	// hlog(LOG_DEBUG, "filter '%s'  rc=%d", f->h.text, rc);

	return rc;
}

int filter_process(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	struct filter_t *f = c->defaultfilters;

	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		// no reports to user about bad filters..
		if (rc > 0)
			return (rc == 1);
			// "2" reply means: "match, but don't pass.."
	}

	f = c->userfilters;

	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		if (rc < 0) {
			rc = client_bad_filter_notify(self, c, f->h.text);
			if (rc < 0) // possibly the client got destroyed here!
				return rc;
		}
		if (rc > 0)
			return (rc == 1);
			// "2" reply means: "match, but don't pass.."
	}
	return 0;
}
