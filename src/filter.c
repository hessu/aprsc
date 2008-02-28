#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "hmalloc.h"
#include "hlog.h"
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
	char	type;	/* 1 char			*/
	char	negate;	/* boolean flag			*/
	const char *text; /* filter text as is		*/
	float   f[4];	/* parsed floats, if any	*/
};


int filter_parse(struct client_t *c, char *filt)
{
	struct filter_t *f, f0;
	struct filter_t **ff = & c->filterhead;
	int i;

	memset(&f0, 0, sizeof(f0));
	if (*filt == '-') {
	  f0.negate = 1;
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
			 &f0.f[0],&f0.f[1],&f0.f[2],&f0.f[3]);
	  if (i != 4) return -1;
	  if (!( -90.01 < f0.f[0] && f0.f[0] <  90.01)) return -2;
	  if (!(-180.01 < f0.f[1] && f0.f[1] < 180.01)) return -2;
	  if (!( -90.01 < f0.f[2] && f0.f[2] <  90.01)) return -2;
	  if (!(-180.01 < f0.f[3] && f0.f[3] < 180.01)) return -2;
	  if (f0.f[0] < f0.f[2]) return -3; /* expect: latN >= latS */
	  if (f0.f[1] > f0.f[3]) return -3; /* expect: lonW <= lonE */
	  f0.f[0] *= (3.1415926/180.0); /* deg-to-radians */
	  f0.f[1] *= (3.1415926/180.0); /* deg-to-radians */
	  f0.f[2] *= (3.1415926/180.0); /* deg-to-radians */
	  f0.f[3] *= (3.1415926/180.0); /* deg-to-radians */
	  break;
	case 'r':
	  /*  r/lat/lon/dist            Range filter  */

	  i = sscanf(filt, "r/%f/%f/%f",
			 &f0.f[0],&f0.f[1],&f0.f[2]);
	  if (i != 3) return -1;
	  if (!( -90.01 < f0.f[0] && f0.f[0] <  90.01)) return -2;
	  if (!(-180.01 < f0.f[1] && f0.f[1] < 180.01)) return -2;
	  f0.f[0] *= (3.1415926/180.0); /* deg-to-radians */
	  f0.f[1] *= (3.1415926/180.0); /* deg-to-radians */
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
	return 1; /* for now pass all */
}
