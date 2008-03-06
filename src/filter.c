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

#include <string.h>
#include <strings.h>
#include <ctype.h>

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

struct filter_head_t {
	struct filter_t *next;
	const char *text; /* filter text as is		*/
	float   f_latN, f_lonE, f_latS, f_lonW;
			/* parsed floats, if any	*/
#define f_dist   f_latS /* for R filter */
#define f_coslat f_lonW /* for R filter */

	char	type;	  /* 1 char			*/
	char	negation; /* boolean flag		*/
};
struct filter_t {
	struct filter_head_t h;
#define FILT_TEXTBUFSIZE (64-sizeof(struct filter_head_t))
	char textbuf[FILT_TEXTBUFSIZE];
};


cellarena_t *filter_cells;


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
	filter_cells = cellinit( sizeof(struct filter_t), __alignof__(struct filter_t),
				 1 /* LIFO ! */, 128 /* 128 kB at the time */,
				 0 );

	/* printf("filter: sizeof=%d alignof=%d\n",sizeof(struct filter_t),__alignof__(struct filter_t)); */
}


int filter_parse(struct client_t *c, const char *filt, int is_user_filter)
{
	struct filter_t *f, f0;
	int i;
	const char *filt0 = filt;
	char dummyc;
	struct filter_t **ff;
	char dummyb[30];

	if (is_user_filter)
		ff = & c->userfilters;
	else
		ff = & c->defaultfilters;

	memset(&f0, 0, sizeof(f0));
	if (*filt == '-') {
		f0.h.negation = 1;
		++filt;
	}
	f0.h.type = *filt;

	if (!strchr("abdeopqrstu",*filt)) {
		/* Not valid filter code */
		hlog(LOG_DEBUG, "Bad filter: %s", filt0);
		return -1;
	}

	switch (f0.h.type) {
	case 'a':
	case 'A':
		/*  a/latN/lonW/latS/lonE     Area filter  */

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
	case 'r':
	case 'R':
		/*  r/lat/lon/dist            Range filter  */

		i = sscanf(filt, "r/%f/%f/%f",
			 &f0.h.f_latN, &f0.h.f_lonW, &f0.h.f_dist);
		if (i != 3) {
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

	case 'f':
	case 'F':
		/*  f/call/dist            Friend's range filter  */

		i = sscanf(filt, "r/%*9[^/]/%f",
			   &f0.h.f_dist);
		if (i != 1) {
			hlog(LOG_DEBUG, "Bad parse: %s", filt0);
			return -1;
		}

		hlog(LOG_DEBUG, "Filter: %s -> F xxx %.3f", filt0, f0.h.f_dist);

		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonW = filter_lon2rad(f0.h.f_lonW);

		f0.h.f_coslat = cosf( f0.h.f_latN ); /* Store pre-calculated COS of LAT */
		break;

	case 'm':
	case 'M':
		/*  m/dist            My range filter  */

		i = sscanf(filt, "r/%f",
			   &f0.h.f_dist);
		if (i != 1) {
			hlog(LOG_DEBUG, "Bad parse: %s", filt0);
			return -1;
		}

		hlog(LOG_DEBUG, "Filter: %s -> M %.3f", filt0, f0.h.f_dist);

		f0.h.f_latN = filter_lat2rad(f0.h.f_latN);
		f0.h.f_lonW = filter_lon2rad(f0.h.f_lonW);

		f0.h.f_coslat = cosf( f0.h.f_latN ); /* Store pre-calculated COS of LAT */
		break;

#if 0
	case 't':
	case 'T':
		/* t/..............
		   t/............../call/km
		*/
		char *s = filt+1;
		if (*s++ != '/') {
			hlog(LOG_DEBUG, "Bad parse: %s", filt0);
			return -1;
		}
		for ( ; *s && *s != '/'; ++s ) {
			if (!strchr("poimntqsuc*",*s)) {
				hlog(LOG_DEBUG, "Bad parse: %s", filt0);
				return -1;
			}
		}
		if (*s == '/') { /* second format */
		  
		}

		break;
#endif

	default:;
		// No pre-parsers for other types
		hlog(LOG_DEBUG, "Filter: %s", filt0);
		break;
	}
	
	/* OK, pre-parsing produced accepted result */

	f = cellmalloc(filter_cells);
	if (!f) return -1;
	*f = f0; /* store pre-parsed values */
	if (strlen(filt) < FILT_TEXTBUFSIZE) {
		strcpy(f->textbuf, filt);
		f->h.text = f->textbuf;
	} else
		f->h.text = hstrdup(filt); /* and copy of filter text */

	/* link to the tail.. */
	while (*ff != NULL)
		ff = &((*ff)->h.next);
	*ff = f;

	return 0;
}

/* Discard the defined filter chain */
void filter_free(struct filter_t *f)
{
	struct filter_t *fnext;

	for ( ; f ; f = fnext ) {
		fnext = f->h.next;
		if (f->h.text) {
			/* If not pointer to internal string, free it.. */
			if (f->h.text != f->textbuf)
				hfree((void*)(f->h.text));
		}
		cellfree(filter_cells, f);
	}
}


int wildpatternmatch(const char *keybuf, const char *p, int negation)
{
	/* Implements:   b/call1/call2...  	Budlist filter, et.al.
	   Pass all traffic FROM exact call: call1, call2, ...
	   (* wild card allowed)

	   p points to first char of "call1" above.
	*/

	const char *k;
	while (*p)  {
	  k = keybuf;
	  while (*p == *k && *k != 0) {
	    ++p; ++k;
	  }
	  if (*k != 0 && *p == '*')
	    return negation ? 2 : 1; // WILD MATCH!
	  if (*k == 0 && (*p == '/' || *p == 0))
	    return negation ? 2 : 1; // Exact match

	  // No match, scan for next pattern
	  while (*p && *p != '/')
	    ++p;
	  if (*p == '/')
	    ++p;
	  // If there is more of patterns, the loop continues..
	}
	return 0;
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

float maidenhead_km_distance(float lat1, float coslat1, float lon1, float lat2, float coslat2, float lon2)
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

int filter_process_one_r(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* r/lat/lon/dist  	Range filter

	   Pass posits and objects within dist km from lat/lon.
	   lat and lon are signed degrees, i.e. negative for West/South
	   and positive for East/North.

	   Multiple range filters can be defined at the same time to allow better coverage.

	   Messages addressed to stations within the range are also passed.
	*/

	float lat1    = f->h.f_latN;
	float lon1    = f->h.f_lonE;
	float coslat1 = f->h.f_coslat;
	float r       = 50000.0; // way too much km ...

	float lat2, lon2, coslat2;

	if (pb->flags & F_HASPOS) {
		lat2    = pb->lat;
		lon2    = pb->lng;
		coslat2 = pb->cos_lat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);
	}
	if (pb->packettype & T_MESSAGE) {
		// Messages to stations within range...
		int i;
		char keybuf[CALLSIGNLEN_MAX+1];
		char *s;
		struct history_cell_t *history;

		keybuf[CALLSIGNLEN_MAX] = 0;
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
		s = strchr(keybuf, ':'); // per specs should not be found, but...
		if (s) *s = 0;
		s = keybuf + strlen(keybuf);
		for ( ; s > keybuf; --s ) {  // strip tail space padding..
			if (*s == ' ') *s = 0;
			else break;
		}

		i = historydb_lookup( keybuf, &history );
		if (!i) return 0; // no result

		lat2    = history->lat;
		lon2    = history->lon;
		coslat2 = history->coslat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);
	}
	if (r < f->h.f_dist)  /* Range is less than given limit */
		return (f->h.negation) ? 2 : 1;
	return 0;
}

int filter_process_one_a(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* a/latN/lonW/latS/lonE  	Area filter
	   The area filter works the same as range filter but the filter
	   is defined as a box of coordinates. The coordinates can also
	   been seen as upper left coordinate and lower right. Lat/lon
	   are decimal degrees.   South and west are negative.

	   Multiple area filters can be defined at the same time.

	   Messages addressed to stations within the area are also passed.
	*/
	;
	if (pb->flags & F_HASPOS) {
		if ((pb->lat <= f->h.f_latN) &&
		    (pb->lat >= f->h.f_latS) &&
		    (pb->lng <= f->h.f_lonE) && /* East POSITIVE ! */
		    (pb->lng >= f->h.f_lonW)) {
			/* Inside the box */
			return f->h.negation ? 2 : 1;
		}
	}
	if (pb->packettype & T_MESSAGE) {
		// Messages to stations within area...
		int i;
		char keybuf[CALLSIGNLEN_MAX+1];
		char *s;
		struct history_cell_t *history;

		float lat1, lon1, coslat1;
		float lat2, lon2, coslat2;


		keybuf[CALLSIGNLEN_MAX] = 0;
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
		s = strchr(keybuf, ':'); // per specs should not be found, but...
		if (s) *s = 0;
		s = keybuf + strlen(keybuf);
		for ( ; s > keybuf; --s ) {  // strip tail space padding..
			if (*s == ' ') *s = 0;
			else break;
		}

		i = historydb_lookup( keybuf, &history );
		if (!i) return 0; // no result

		lat1    = f->h.f_latN;
		lon1    = f->h.f_lonE;
		coslat1 = f->h.f_coslat;

		lat2    = history->lat;
		lon2    = history->lon;
		coslat2 = history->coslat;

		if ((pb->lat <= f->h.f_latN) ||
		    (pb->lat >= f->h.f_latS) ||
		    (pb->lng <= f->h.f_lonE) || /* East POSITIVE ! */
		    (pb->lng >= f->h.f_lonW)) {
			/* Inside the box */
			return f->h.negation ? 2 : 1;
		}
	}
	return 0;
}

int filter_process_one_b(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* b/call1/call2...  	Budlist filter
	   Pass all traffic FROM exact call: call1, call2, ...
	   (* wild card allowed)
	*/

	const char *p = f->h.text + 2;
	char keybuf[CALLSIGNLEN_MAX+1];
	int i = pb->srccall_end+1 - pb->data;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// source address  "addr>"
	memcpy( keybuf, pb->data, i);
	keybuf[i] = 0;

	return wildpatternmatch(keybuf, p, f->h.negation);
}

int filter_process_one_u(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* u/unproto1/unproto2/...  	Unproto filter

	   This filter passes all packets with the specified destination
	   callsign-SSID(s) (also known as the To call or unproto call).
	   Supports * wild card.
	*/

	const char *p = f->h.text + 2;
	char keybuf[CALLSIGNLEN_MAX+1];
	int i = pb->dstcall_end - (pb->srccall_end+1);

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// destination address  ">addr,"
	memcpy( keybuf, pb->srccall_end+1, i);
	keybuf[i] = 0;

	return wildpatternmatch(keybuf, p, f->h.negation);
}

int filter_process_one_d(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* d/digi1/digi2...  	Digipeater filter
	   The digipeater filter will pass all packets that have been
	   digipeated by a particular station(s) (the station's call
	   is in the path).   This filter allows the * wildcard.
	*/


	return 0;
}

int filter_process_one_e(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* e/call1/call1/...  	Entry station filter
	   This filter passes all packets with the specified
	   callsign-SSID(s) immediately following the q construct.
	   This allows filtering based on receiving IGate, etc.
	   Supports * wildcard.
	*/
	return 0;
}

int filter_process_one_o(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* o/obj1/obj2...  	Object filter
	   Pass all objects with the exact name of obj1, obj2, ...
	   (* wild card allowed)
	*/
	const char *p = f->h.text + 2;
	char keybuf[CALLSIGNLEN_MAX+1];
	char *s;

	if (pb->packettype & T_OBJECT) {
		// Pick object name  ";item  *"
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
		s = strchr(keybuf, '*');
		if (s) *s = 0;
		else {
			s = strchr(keybuf, '_');
			if (s) {
				*s = 0;
			}
			// Will also pass object-kill messages
		}
		s = keybuf + strlen(keybuf);
		for ( ; s > keybuf; --s ) {  // strip tail space padding
			if (*s == ' ') *s = 0;
			else break;
		}

		return wildpatternmatch(keybuf, p, f->h.negation);

	}
	return 0;
}

int filter_process_one_p(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{

	/* p/aa/bb/cc...  	Prefix filter
	   Pass traffic with fromCall that start with aa or bb or cc...
	*/

	/* Implements:   b/call1/call2...  	Budlist filter, et.al.
	   Pass all traffic FROM exact call: call1, call2, ...
	   (* wild card allowed)

	   p points to first char of "call1" above.
	*/

	const char *p = f->h.text + 2;
	char keybuf[CALLSIGNLEN_MAX+1];
	int i = pb->srccall_end+1 - pb->data;
	const char *k;

	if (i > CALLSIGNLEN_MAX) i = CALLSIGNLEN_MAX;

	// source address  "addr>"
	memcpy( keybuf, pb->data, i);
	keybuf[i] = 0;

	while (*p)  {
	  k = keybuf;
	  while (*p == *k && *k != 0 && *k != '/') {
	    ++p; ++k;
	  }
	  if (*k != 0 && *p == '/')
	    return f->h.negation ? 2 : 1; // PREFIX match
	  if (*k == 0 && (*p == '/' || *p == 0))
	    return f->h.negation ? 2 : 1; // Exact match

	  // No match, scan for next pattern
	  while (*p && *p != '/')
	    ++p;
	  if (*p == '/')
	    ++p;
	  // If there is more of patterns, the loop continues..
	}

	return 0;
}

int filter_process_one_q(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
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
	return 0;
}

int filter_process_one_s(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* s/pri/alt/over  	Symbol filter  	pri = symbols in primary table

	   alt = symbols in alternate table
	   over = overlay character (case sensitive)

	   For example:
	   s/->   This will pass all House and Car symbols (primary table)
	   s//#   This will pass all Digi with or without overlay
	   s//#/T This will pass all Digi with overlay of capital T
	*/
	return 0;
}

int filter_process_one_t(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
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
	const char *t = f->h.text+2;
	int rc = 0;
	for ( ; *t && *t != '/' ; ++t) { /* Go thru all matchers */
		switch (*t) {
		case '*': /* 'ALL' ? */
			rc = 1;
			break;

		case 'c':
		case 'C':
			if (pb->packettype & T_CWOP)
				rc = 1;
			break;

		case 'p':
		case 'P':
			if (pb->packettype & T_POSITION)
				rc = 1;
			break;

		case 'o':
		case 'O':
			if (pb->packettype & T_OBJECT)
				rc = 1;
			break;

		case 'i':
		case 'I':
			if (pb->packettype & T_ITEM)
				rc = 1;
			break;

		case 'm':
		case 'M':
			if (pb->packettype & T_MESSAGE)
				rc = 1;
			break;

		case 'n':
		case 'N':
			if (pb->packettype & T_NWS)
				rc = 1;
			break;

		case 'w':
		case 'W':
			if (pb->packettype & T_WX)
				rc = 1;
			break;

		case 't':
		case 'T':
			if (pb->packettype & T_TELEMETRY)
				rc = 1;
			break;

		case 'q':
		case 'Q':
			if (pb->packettype & T_QUERY)
				rc = 1;
			break;

		case 's':
		case 'S':
			if (pb->packettype & T_STATUS)
				rc = 1;
			break;

		case 'u':
		case 'U':
			if (pb->packettype & T_USERDEF)
				rc = 1;
			break;

		default:
			break;
		}
	}

	/* Either it stops here, or it continues... */

	if (*t == '/') { /* Within a range of callsign ? */
		char callsign[20], *s;
		float range;
		++t;
		s = memccpy(callsign, t, '/', 19);
		if (s) *s = 0;
		else /* BAD PARSE! */
			return -2;
		t = strchr(t, '/');
		if (!t) /* BAD PARSE! */
			return -2;
		++t;
		if (sscanf(t,"%f", &range) != 1)
			return -2;  /* BAD PARSE! */

		if (callsign[0] == 0 || range < 0.0)
			return -2;  /* BAD PARSE! */

		/* So..  Now we have a callsign, and we have range.
		   Lets find callsign's location, and range to that item.. */

		hlog(LOG_DEBUG, "type filter with callsign range used!");
		return 0; /* unimplemented! */
	}

	return (f->h.negation ? -rc : 1);
}

int filter_process_one_f(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* f/call/dist  	Friend Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of call.

	   Multiple friend filters can be defined at the same time.

	   Messages addressed to stations within the range are also passed.
	*/

	char keybuf[CALLSIGNLEN_MAX+1];
	const char *p;
	char *s;
	int i;
	struct history_cell_t *history;

	float r;
	float lat1, lon1, coslat1;
	float lat2, lon2, coslat2;

	p = f->h.text +2;
	for (i = 0; i < CALLSIGNLEN_MAX; ++i) {
		keybuf[i] = *p;
		if (*p == 0 || *p == '/')
			break;
	}
	keybuf[i] = 0;

	// find friend's last location packet
	i = historydb_lookup( keybuf, &history );
	if (!i) return 0; // no lookup result..

	lat1    = history->lat;
	lon1    = history->lon;
	coslat1 = history->coslat;

	if (pb->flags & F_HASPOS) {

		lat2    = pb->lat;
		lon2    = pb->lng;
		coslat2 = pb->cos_lat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);
	}
	if (pb->packettype & T_MESSAGE) {
		// Messages to stations within range...

		keybuf[CALLSIGNLEN_MAX] = 0;
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
		s = strchr(keybuf, ':'); // per specs should not be found, but...
		if (s) *s = 0;
		s = keybuf + strlen(keybuf);
		for ( ; s > keybuf; --s ) {  // strip tail space padding..
			if (*s == ' ') *s = 0;
			else break;
		}

		i = historydb_lookup( keybuf, &history );
		if (!i) return 0; // no result

		lat2    = history->lat;
		lon2    = history->lon;
		coslat2 = history->coslat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);
	}

	if (r < f->h.f_dist)  /* Range is less than given limit */
		return (f->h.negation) ? 2 : 1;

	return 0;
}

int filter_process_one_m(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* m/dist  	My Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of the logged in client.

	   Messages addressed to stations within the range are also
	   passed.
	*/

	float lat1, lon1, coslat1;
	int i;
	struct history_cell_t *history;

	if (!c->username) // Should not happen...
		return 0;

	i = historydb_lookup( c->username, &history );
	if (!i) return 0; // no result

	lat1    = history->lat;
	lon1    = history->lon;
	coslat1 = history->coslat;

	if (pb->flags & F_HASPOS) {
		float r;
		float lat1, lon1, coslat1;
		float lat2, lon2, coslat2;

		lat2    = f->h.f_latN;
		lon2    = f->h.f_lonE;
		coslat2 = f->h.f_coslat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);
		if (r < f->h.f_dist)  /* Range is less than given limit */
			return f->h.negation ? 2 : 1;
	}
	if (pb->packettype & T_MESSAGE) {
		// Messages to stations within range...
		int i;
		char keybuf[CALLSIGNLEN_MAX+1];
		char *s;
		struct history_cell_t *history;
		float r;
		float lat2, lon2, coslat2;


		keybuf[CALLSIGNLEN_MAX] = 0;
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX);
		s = strchr(keybuf, ':'); // per specs should not be found, but...
		if (s) *s = 0;
		s = keybuf + strlen(keybuf);
		for ( ; s > keybuf; --s ) {  // strip tail space padding..
			if (*s == ' ') *s = 0;
			else break;
		}

		i = historydb_lookup( keybuf, &history );
		if (!i) return 0; // no result

		lat2    = history->lat;
		lon2    = history->lon;
		coslat2 = history->coslat;

		r = maidenhead_km_distance(lat1, coslat1, lon1, lat2, coslat2, lon2);

		if (r < f->h.f_dist)  /* Range is less than given limit */
			return (f->h.negation) ? 2 : 1;
	}

	return 0;
}


int filter_process_one(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
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

	return rc;
}

int filter_process(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	struct filter_t *f = c->defaultfilters;

	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		if (rc > 0)
			return (rc == 1); // "2" reply means: "match, but don't pass.."
	}

	f = c->userfilters;

	for ( ; f; f = f->h.next ) {
		int rc = filter_process_one(c, pb, f);
		if (rc < 0)
			client_bad_filter_notify(self, c, f->h.text);
		if (rc > 0)
			return (rc == 1); // "2" reply means: "match, but don't pass.."
	}
	return 0;
}
