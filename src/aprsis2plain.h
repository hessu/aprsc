/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

/*
 *	aprsis2plain.h: IS2 metadata from plaintext clients
 */

#ifndef APRSIS2PLAIN_H
#define APRSIS2PLAIN_H

#include "worker.h"

extern void is2_plain_metadata_reset(struct client_t *c);
extern int is2_plain_metadata(struct worker_t *self, struct client_t *c, const char *s, int len);

typedef void (*is2_kv_cb_t)(void *ctx, const char *key, int key_len, const char *val, int val_len);
extern void is2_kv_parse(const char *s, int len, is2_kv_cb_t key_cb, void *ctx);
extern int is2_kv_version_pick(const char *val, int val_len, int min, int max);
extern int is2_kv_int(const char *val, int val_len, long min, long max, long *out);

#endif
