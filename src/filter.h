#ifndef FILTER_H
#define FILTER_H 1

#include "worker.h"


extern int filter_parse(struct client_t *c, char *filt);
extern void filter_free(struct filter_t *c);
extern int filter_process(struct client_t *c, struct pbuf_t *pb);

extern float filter_lat2rad(float lat);
extern float filter_lon2rad(float lon);

#endif /* FILTER_H */
