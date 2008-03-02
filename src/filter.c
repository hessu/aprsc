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

/*
  See:  http://www.aprs-is.net/javaprssrvr/javaprsfilter.htm


  a/latN/lonW/latS/lonE Area filter
  b/call1/call2...  	Budlist filter (*)
  d/digi1/digi2...  	Digipeater filter (*)
  e/call1/call1/...  	Entry station filter (*)
  o/obj1/obj2...  	Object filter (*)
  p/aa/bb/cc...  	Prefix filter
  q/con/ana 	 	q Contruct filter
  s/pri/alt/over  	Symbol filter
  t/poimntqsu		Type filter
  u/unproto1/unproto2/.. Unproto filter (*)
  r/lat/lon/dist  	Range filter

  f/call/dist  		Friend Range filter (NOT supported)
  m/dist  		My Range filter (NOT supported)
  t/poimntqsu/call/km	Type filter (NOT supported)

  (*) = wild-card supported

 */


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
				 1 /* LIFO ! */, 128 /* 128 kB at the time */ );

	/* printf("filter: sizeof=%d alignof=%d\n",sizeof(struct filter_t),__alignof__(struct filter_t)); */
}


int filter_parse(struct client_t *c, const char *filt)
{
	struct filter_t *f, f0;
	struct filter_t **ff = & c->filterhead;
	int i;
	const char *filt0 = filt;
	char dummyc;

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

	default:;
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

float maidenhead_km_distance(struct filter_t *f, struct pbuf_t *pb)
{
	float dlat = f->h.f_latN - pb->lat;
	float dlon = f->h.f_lonE - pb->lng;
	float sindlat2 = sinf(dlat * 0.5);
	float sindlon2 = sinf(dlon * 0.5);
	float a, c;

	if (!(pb->cos_lat)) { /* gets re-calculated only at exact poles: +- pi/2 */
		pb->cos_lat = cosf(pb->lat);
	}

	a = (sindlat2 * sindlat2 +
	     f->h.f_coslat * pb->cos_lat * sindlon2 * sindlon2);

	c = 2.0 * atan2f( sqrtf(a), sqrtf(1.0 - a));

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
	   and positive for East/North. Up to 9 range filters can be
	   defined at the same time to allow better coverage.

	   Messages addressed to stations within the range are also
	   passed.   ## NOT IMPLEMENTED ##
	*/

	if (pb->packettype & T_POSITION) {
	  float r = maidenhead_km_distance(f, pb);
	  if ((!f->h.negation) && (r < f->h.f_dist))
	    return 1;  /* Range is less than given limit */
	  if ((f->h.negation)  && (r > f->h.f_dist))
	    return 1;  /* Range is greater than given limit */
	}
	return 0;
}

int filter_process_one_a(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* a/latN/lonW/latS/lonE  	Area filter
	   The area filter works the same as range filter but the filter
	   is defined as a box of coordinates. The coordinates can also
	   been seen as upper left coordinate and lower right. Lat/lon
	   are decimal degrees.   South and west are negative.
	   Up to 9 area filters can be defined at the same time.
	*/
	if (pb->packettype & T_POSITION) {
		if ((pb->lat > f->h.f_latN) ||
		    (pb->lat < f->h.f_latS) ||
		    (pb->lng > f->h.f_lonE) || /* East POSITIVE ! */
		    (pb->lng < f->h.f_lonW)) {
			/* Outside the box */
			if (f->h.negation)
				return 1;
		} else {
			/* Inside the box */
			if (!f->h.negation)
				return 1;
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
	return 0;
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
	return 0;
}

int filter_process_one_p(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{

	/* p/aa/bb/cc...  	Prefix filter
	   Pass traffic with fromCall that start with aa or bb or cc...
	*/
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
int filter_process_one_u(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* u/unproto1/unproto2/...  	Unproto filter

	   This filter passes all packets with the specified destination
	   callsign-SSID(s) (also known as the To call or unproto call).
	   Supports * wildcard.
	*/
	return 0;
}

int filter_process_one_t(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* t/poimntqsu
	   t/poimntqsu/call/km  (NOT supported?)

	   Type filter 	Pass all traffic based on packet type.
	   One or more types can be defined at the same time, t/otq
	   is a valid definition.

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
	*/
	return 0;
}

int filter_process_one_f(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* f/call/dist  	Friend Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of call.  Up to 9 friend
	   filters can be defined at the same time.
	*/

	return 0;
}

int filter_process_one_m(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	/* m/dist  	My Range filter
	   This is the same as the range filter except that the center is
	   defined as the last known position of the logged in client.
	*/
	return 0;
}


int filter_process_one(struct client_t *c, struct pbuf_t *pb, struct filter_t *f)
{
	switch (f->h.type) {

	case 'a':
		if (filter_process_one_a(c, pb, f))
			return 1;
		break;

	case 'b':
		if (filter_process_one_b(c, pb, f))
			return 1;
		break;

	case 'd':
		if (filter_process_one_d(c, pb, f))
			return 1;
		break;

	case 'e':
		if (filter_process_one_e(c, pb, f))
			return 1;
		break;

	case 'f':
		if (filter_process_one_f(c, pb, f))
			return 1;
		break;

	case 'm':
		if (filter_process_one_m(c, pb, f))
			return 1;
		break;

	case 'o':
		if (filter_process_one_o(c, pb, f))
			return 1;
		break;

	case 'p':
		if (filter_process_one_p(c, pb, f))
			return 1;
		break;

	case 'q':
		if (filter_process_one_q(c, pb, f))
			return 1;
		break;

	case 'r':
		if (filter_process_one_r(c, pb, f))
			return 1;
		break;

	case 's':
		if (filter_process_one_s(c, pb, f))
			return 1;
		break;

	case 't':
		if (filter_process_one_t(c, pb, f))
			return 1;
		break;

	case 'u':
		if (filter_process_one_u(c, pb, f))
			return 1;
		break;

	default:
		break;
	}

	return 0;
}

int filter_process(struct client_t *c, struct pbuf_t *pb)
{
	struct filter_t *f = c->filterhead;

	for ( ; f; f = f->h.next ) {
		if (filter_process_one(c, pb, f))
			return 1;
	}
	return 1;  /* for now pass all */
}
