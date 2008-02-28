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
	int	type;	/* 1 char			*/
	const char *text; /* filter text as is		*/
	float   f[4];	/* parsed floats, if any	*/
};


int filter_parse(struct client_t *c, char *filt)
{
	struct filter_t *f = hmalloc(sizeof(*f));
	struct filter_t **ff = & c->filterhead;
	int i;

	if (!f) return -1;
	memset(f, 0, sizeof(*f));
	f->text = hstrdup(filt);
	f->type = *filt;

	/* link to the tail.. */
	while (*ff != NULL)
	  ff = &((*ff)->next);
	*ff = f;

	if (!strchr("abdeopqrstu",*filt)) {
	  /* Not valid filter code */
	  return -1;
	}

	switch (f->type) {
	case 'a':
	  i = sscanf(filt, "a/%f/%f/%f/%f",
			 &f->f[0],&f->f[1],&f->f[2],&f->f[3]);
	  if (i != 4) return -1;
	  return 0;
	  break;
	case 'r':
	  i = sscanf(filt, "r/%f/%f/%f",
			 &f->f[0],&f->f[1],&f->f[2]);
	  if (i != 3) return -1;
	  return 0;
	  break;
	default:;
	  break;
	}
	
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
