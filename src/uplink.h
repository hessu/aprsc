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

#ifndef UPLINK_H
#define UPLINK_H 1

#include "worker.h"

extern int uplink_reconfiguring;
extern int uplink_shutting_down;

extern void uplink_thread(void *asdf);
extern void uplink_close(struct client_t *c);
extern void uplink_start(void);
extern void uplink_stop(void);
extern int  uplink_login_handler(struct worker_t *self, struct client_t *c, char *s, int len);

#endif /* FILTER_H */
