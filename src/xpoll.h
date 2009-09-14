/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef XPOLL_H
#define XPOLL_H

#define XP_USE_POLL

#ifdef XP_USE_POLL
#define XP_INCREMENT 64 // The "struct pollfd" is _small_, avoid mem fragment
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

extern struct xpoll_t *xpoll_initialize(struct xpoll_t *xp, void *tp, int (*handler) (struct xpoll_t *xp, struct xpoll_fd_t *xfd));
extern int xpoll_free(struct xpoll_t *xp);
extern struct xpoll_fd_t *xpoll_add(struct xpoll_t *xp, int fd, void *p);
extern int xpoll_remove(struct xpoll_t *xp, struct xpoll_fd_t *xfd);
extern void xpoll_outgoing(struct xpoll_t *xp, struct xpoll_fd_t *xfd, int have_outgoing);
extern int xpoll(struct xpoll_t *xp, int timeout);
extern void xpoll_init(void);

#endif
