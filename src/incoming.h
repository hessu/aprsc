/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
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

#ifndef INCOMING_H
#define INCOMING_H

#include "worker.h"

extern char *memstr(char *needle, char *haystack, char *haystack_end);

extern void incoming_flush(struct worker_t *self);
extern int incoming_handler(struct worker_t *self, struct client_t *c, char *s, int len);
extern int incoming_uplinksim_handler(struct worker_t *self, struct client_t *c, char *s, int len);

#endif

