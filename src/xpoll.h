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

#ifndef XPOLL_H
#define XPOLL_H

#define XP_USE_POLL

#ifdef XP_USE_POLL
#define XP_INCREMENT 2
#include <poll.h>
#endif

#define XP_IN	1
#define XP_OUT	2
#define XP_ERR	4

struct xpoll_fd_t {
	int fd;
	void *p;	/* a fd-specific pointer, which will be passed to handlers */
	
	int result;

#ifdef XP_USE_POLL
	int pollfd_n;	/* index to xp->pollfd[] */
#endif

	struct xpoll_fd_t *next;
	struct xpoll_fd_t **prevp;
};

struct xpoll_t {
	void *tp;	/* an xpoll_t-specific pointer, which will be passed to handlers */
	struct xpoll_fd_t *fds;
	
	int	(*handler)	(struct xpoll_t *xp, struct xpoll_fd_t *xfd);

#ifdef XP_USE_POLL
	struct pollfd *pollfd;
	int pollfd_len;
	int pollfd_used;
#endif

};

extern struct xpoll_t *xpoll_init(void *tp, int (*handler) (struct xpoll_t *xp, struct xpoll_fd_t *xfd));
extern int xpoll_close(struct xpoll_t *xp);
extern struct xpoll_fd_t *xpoll_add(struct xpoll_t *xp, int fd, void *p);
extern int xpoll_remove(struct xpoll_t *xp, struct xpoll_fd_t *xfd);
extern void xpoll_outgoing(struct xpoll_t *xp, struct xpoll_fd_t *xfd, int have_outgoing);
extern int xpoll(struct xpoll_t *xp, int timeout);

#endif
