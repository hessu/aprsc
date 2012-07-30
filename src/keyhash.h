/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *
 */

#ifndef KEYHASH_H
#define KEYHASH_H

extern void     keyhash_init(void);
extern uint32_t keyhash(const void *s, int slen, uint32_t hash0);
extern uint32_t keyhashuc(const void *s, int slen, uint32_t hash0);

#endif
