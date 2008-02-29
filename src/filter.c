#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <math.h>

#include "hmalloc.h"
#include "hlog.h"
#include "worker.h"
#include "filter.h"

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


struct filter_t {
	struct filter_t *next;
	char	type;	  /* 1 char			*/
	char	negation; /* boolean flag		*/
	const char *text; /* filter text as is		*/
	float   f_latN, f_lonE, f_latS, f_lonW;
			/* parsed floats, if any	*/
#define f_dist   f_latS /* for R filter */
#define f_coslat f_lonW /* for R filter */
};




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
	float dlat = f->f_latN - pb->lat;
	float dlon = f->f_lonE - pb->lng;
	float sindlat2 = sinf(dlat * 0.5);
	float sindlon2 = sinf(dlon * 0.5);
	float a, c;

	if (!(pb->cos_lat)) { /* gets re-calculated only at exact poles: +- pi/2 */
	  pb->cos_lat = cosf(pb->lat);
	}

	a = (sindlat2 * sindlat2 +
	     f->f_coslat * pb->cos_lat * sindlon2 * sindlon2);

	c = 2.0 * atan2f( sqrtf(a), sqrtf(1.0 - a));

	return ((111.2 * 180.0 / M_PI) * c);
}



float filter_lat2rad(float lat)
{
	/* return atan(gm4anb_b2pera2 * tan(lat * (M_PI / 180.0))); */
	return (lat * (M_PI / 180.0));
}

float filter_lon2rad(float lon)
{
	return (lon * (M_PI / 180.0));
}


int filter_parse(struct client_t *c, char *filt)
{
	struct filter_t *f, f0;
	struct filter_t **ff = & c->filterhead;
	int i;

	memset(&f0, 0, sizeof(f0));
	if (*filt == '-') {
	  f0.negation = 1;
	  ++filt;
	}
	f0.type = *filt;

	if (!strchr("abdeopqrstu",*filt)) {
	  /* Not valid filter code */
	  return -1;
	}

	switch (f0.type) {
	case 'a':
	  /*  a/latN/lonW/latS/lonE     Area filter  */

	  i = sscanf(filt, "a/%f/%f/%f/%f",
		     &f0.f_latN, &f0.f_lonW, &f0.f_latS, &f0.f_lonE);
	  if (i != 4) return -1;

	  if (!( -90.01 < f0.f_latN && f0.f_latN <  90.01)) return -2;
	  if (!(-180.01 < f0.f_lonW && f0.f_lonW < 180.01)) return -2;
	  if (!( -90.01 < f0.f_latS && f0.f_latS <  90.01)) return -2;
	  if (!(-180.01 < f0.f_lonE && f0.f_lonE < 180.01)) return -2;

	  if (f0.f_latN < f0.f_latS) return -3; /* expect: latN >= latS */
	  if (f0.f_lonW > f0.f_lonE) return -3; /* expect: lonW <= lonE */

	  f0.f_latN = filter_lat2rad(f0.f_latN);
	  f0.f_lonW = filter_lon2rad(f0.f_lonW);

	  f0.f_latS = filter_lat2rad(f0.f_latS);
	  f0.f_lonE = filter_lon2rad(f0.f_lonE);

	  break;
	case 'r':
	  /*  r/lat/lon/dist            Range filter  */

	  i = sscanf(filt, "r/%f/%f/%f",
			 &f0.f_latN, &f0.f_lonW, &f0.f_dist);
	  if (i != 3) return -1;

	  if (!( -90.01 < f0.f_latN && f0.f_latN <  90.01)) return -2;
	  if (!(-180.01 < f0.f_lonW && f0.f_lonW < 180.01)) return -2;

	  f0.f_latN = filter_lat2rad(f0.f_latN);
	  f0.f_lonW = filter_lon2rad(f0.f_lonW);

	  f0.f_coslat = cosf( f0.f_latN ); /* Store pre-calculated COS of LAT */
	  break;

	default:;
	  break;
	}
	
	/* OK, pre-parsing produced accepted result */

	f = hmalloc(sizeof(*f));
	if (!f) return -1;
	*f = f0; /* store pre-parsed values */
	f->text = hstrdup(filt); /* and copy of filter text */

	/* link to the tail.. */
	while (*ff != NULL)
	  ff = &((*ff)->next);
	*ff = f;

	return 0;
}

/* Discard the defined filter chain */
void filter_free(struct filter_t *f)
{
	struct filter_t *fnext;

	for ( ; f ; f = fnext ) {
	  fnext = f->next;
	  if (f->text) hfree((void*)(f->text));
	  hfree(f);
	}
}


int filter_process(struct client_t *c, struct pbuf_t *pb)
{
	struct filter_t *f = c->filterhead;

	for ( ; f; f = f->next ) {
	  switch (f->type) {
	  case 'a': /* Area filters */
	    if (pb->packettype & T_POSITION) {
	      if ((pb->lat > f->f_latN) ||
		  (pb->lat < f->f_latS) ||
		  (pb->lng > f->f_lonE) || /* East POSITIVE ! */
		  (pb->lng < f->f_lonW)) {
		/* Outside the box */
		if (f->negation)
		  return 1;
	      } else {
		/* Inside the box */
		if (!f->negation)
		  return 1;
	      }
	    }
	    break;

	  case 'r': /* Range filters */
	    if (pb->packettype & T_POSITION) {
	      float r = maidenhead_km_distance(f, pb);
	      if ((!f->negation) && (r < f->f_dist))
		return 1;  /* Range is less than given limit */
	      if ((f->negation)  && (r > f->f_dist))
		return 1;  /* Range is greater than given limit */
	    }
	    break;

	  case 'b': /* Budlist filter (w/ wild-card support) */
	    break;

	  case 'd': /* Digipeater filter (w/ wild-card support) */
	    break;

	  case 'e': /* Entry station filter (w/ wild-card support) */
	    break;

	  case 'o': /* Object filter (w/ wild-card support) */
	    break;

	  case 'p': /* Prefix filter */
	    break;

	  case 'q': /* q Construct filter */
	    break;

	  case 's': /* Symbol filter */
	    break;

	  case 'u': /* Unproto filter (w/ wild-card support) */
	    break;

	  case 't': /* Type filter (partial support) */
	    break;

	  case 'f': /* Friend Range filter (NOT supported?) */
	    break;

	  case 'm': /* My Range filter (NOT supported?) */
	    break;

	  default:
	    break;
	  }
	}

	return 1; /* for now pass all */
}
