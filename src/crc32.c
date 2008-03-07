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
/*
 *	Copyright 1988 by Rayan S. Zachariassen, all rights reserved.
 *	This will be free software, but only when it is finished.
 */

/*
 *
 * CRC32 code extracted out from  symbol.c  into its own file
 * by Matti Aarnio <mea@nic.funet.fi> 9-Sept-1999
 *
 */

#include <stdint.h>
#include <sys/types.h>

/* crc table and hash algorithm from pathalias */
/*
 * fold a string into a long int.  31 bit crc (from andrew appel).
 * the crc table is computed at run time by crcinit() -- we could
 * precompute, but it takes 1 clock tick on a 750.
 *
 * This fast table calculation works only if POLY is a prime polynomial
 * in the field of integers modulo 2.  Since the coefficients of a
 * 32-bit polynomail won't fit in a 32-bit word, the high-order bit is
 * implicit.  IT MUST ALSO BE THE CASE that the coefficients of orders
 * 31 down to 25 are zero.  Happily, we have candidates, from
 * E. J.  Watson, "Primitive Polynomials (Mod 2)", Math. Comp. 16 (1962):
 *      x^32 + x^7 + x^5 + x^3 + x^2 + x^1 + x^0
 *      x^31 + x^3 + x^0
 *
 * We reverse the bits to get:
 *      111101010000000000000000000000001 but drop the last 1
 *         f   5   0   0   0   0   0   0
 *      010010000000000000000000000000001 ditto, for 31-bit crc
 *         4   8   0   0   0   0   0   0
 */

#include "crc32.h"

#define POLY32 0xf5000000       /* 32-bit polynomial */
#define POLY31 0x48000000       /* 31-bit polynomial */
#define POLY POLY31     /* use 31-bit to avoid sign problems */

static long CrcTable[128];
static int crcinit_done = 0;

void crcinit(void)
{       
	register int i,j;
	register long sum;
	
	if (crcinit_done)
		return;

	for (i = 0; i < 128; i++) {
		sum = 0;
		for (j = 7-1; j >= 0; --j)
			if (i & (1 << j))
				sum ^= POLY >> j;
		CrcTable[i] = sum;
	}
	crcinit_done = 1;
}

/*	Arbitary octet sequence of "slen" bytes.
 *	The scan result is added on value at "key", which
 *	user is expected to initialize as 0.
 */
uint32_t crc32n(const void *p, int slen, uint32_t key)
{
	const unsigned char *s = p;

	if (!crcinit_done)
	  crcinit();

	/* Input string is to be CRCed to form a new key-id */
	for (; slen > 0; ++s, --slen)
	  key = (key >> 7) ^ CrcTable[(key ^ *s) & 0x7f];

	key &= 0xFFFFFFFFUL;

	return key;
}

/* Zero-terminated "string" */
uint32_t crc32(const void *p)
{
	const unsigned char *s = p;

	uint32_t key;

	if (!crcinit_done)
	  crcinit();

	/* Input string is to be CRCed to form a new key-id */
	key = 0;
	for (; *s; ++s)
	  key = (key >> 7) ^ CrcTable[(key ^ *s) & 0x7f];

	key &= 0xFFFFFFFFUL;

	return key;
}
