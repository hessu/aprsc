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

/*
 *	outgoing.c: handle outgoing packets in the worker thread
 */

#include <string.h>

#include "outgoing.h"
#include "hlog.h"
#include "filter.h"

void process_outgoing_single(struct worker_t *self, struct pbuf_t *pb)
{
	struct client_t *c;
	
	for (c = self->clients; (c); c = c->next) {

		/* Do not send to clients that are not logged in. */
		if (c->state != CSTATE_CONNECTED)
			continue;
		// FIXME: on production also CSTATE_UPLINK is to be permitted

		/* Do not send to the same client.
		   This may reject a packet that came from a socket that got
		   closed a few milliseconds previously and its client_t got
		   recycled on a newly connected client, but if the new client
		   is a long living one, all further packets will be accepted
		   just fine.
		   For packet history dumps this test shall be ignored!
		 */
		if (c == pb->origin)
			continue;

		if ((pb->flags & F_DUPE) && !c->want_dupes) {
		  /* Duplicate packet.
		     Don't send, unless client especially wants! */
		  continue;
		}

		/*  Process filters - check if this packet should be
		    sent to this client.   */

		if (filter_process(self, c, pb) > 0) {
			client_write(self, c, pb->data, pb->packet_len);
		}
	}
}
