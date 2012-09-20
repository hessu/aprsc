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
 *	xpoll.c - obfuscate poll(), epoll(), etc under a single API
 *
 *	Please try to keep this module usable as a self-contained library,
 *	having and API of it's own, so that it can be reused without
 *	aprsc easily. Thanks!
 *
 *	TODO: add epoll and kqueue support
 */

#include <string.h>

#include "xpoll.h"
#include "hmalloc.h"
#include "hlog.h"
#include "cellmalloc.h"

#ifndef _FOR_VALGRIND_
cellarena_t *xpoll_fd_pool;
#endif

void xpoll_init(void) {
#ifndef _FOR_VALGRIND_
	xpoll_fd_pool = cellinit( "xpollfd",
				  sizeof(struct xpoll_fd_t),
				  __alignof__(struct xpoll_fd_t),
				  CELLMALLOC_POLICY_FIFO,
				  128 /* 128 kB */,
				  0 /* minfree */ );
#endif
}

struct xpoll_t *xpoll_initialize(struct xpoll_t *xp, void *tp, int (*handler) (struct xpoll_t *xp, struct xpoll_fd_t *xfd))
{
	xp->fds = NULL;
	xp->tp = tp;
 	xp->handler = handler;
	
#ifdef XP_USE_POLL
	xp->pollfd_len = XP_INCREMENT;
	xp->pollfd_used = 0;
	xp->pollfd = hmalloc(sizeof(struct pollfd) * xp->pollfd_len);
#endif
	
	return xp;
}

int xpoll_free(struct xpoll_t *xp)
{
	struct xpoll_fd_t *xfd;
	
	while (xp->fds) {
		xfd = xp->fds->next;
#ifndef _FOR_VALGRIND_
		cellfree( xpoll_fd_pool, xp->fds );
#else
		hfree(xp->fds);
#endif
		xp->fds = xfd;
	}

#ifdef XP_USE_POLL
	hfree(xp->pollfd);
#endif
	return 0;
}

struct xpoll_fd_t *xpoll_add(struct xpoll_t *xp, int fd, void *p)
{
	struct xpoll_fd_t *xfd;

#ifndef _FOR_VALGRIND_
	xfd = (struct xpoll_fd_t*) cellmalloc( xpoll_fd_pool );
	if (!xfd) {
		hlog(LOG_ERR, "xpoll: cellmalloc failed, too many FDs");
		return NULL;
	}
#else
	xfd = (struct xpoll_fd_t*) hmalloc(sizeof(struct xpoll_fd_t));
#endif
	
	xfd->fd = fd;
	xfd->p = p;
	xfd->next = xp->fds;
	xfd->prevp = &xp->fds;
	if (xfd->next)
		xfd->next->prevp = &xfd->next;
	xp->fds = xfd;

#ifdef XP_USE_POLL
	if (xp->pollfd_used >= xp->pollfd_len) {
		/* make the struct longer */
		xp->pollfd_len += XP_INCREMENT;
		xp->pollfd = hrealloc(xp->pollfd, sizeof(struct pollfd) * xp->pollfd_len);
	}
	/* append the new fd to the struct */
	xp->pollfd[xp->pollfd_used].fd = fd;
	xp->pollfd[xp->pollfd_used].events = POLLIN;
	xfd->pollfd_n = xp->pollfd_used;
	xp->pollfd_used++;
#endif

	return xfd;
}

int xpoll_remove(struct xpoll_t *xp, struct xpoll_fd_t *xfd)
{
#ifdef XP_USE_POLL
	/* remove the fd from the pollfd struct by moving the tail of the struct
	 * to the left
	 */
    if (xp->pollfd != NULL) {
	void *to = (char *)xp->pollfd + (xfd->pollfd_n * sizeof(struct pollfd));
	void *from = (char*)to + sizeof(struct pollfd);
	int len = (xp->pollfd_used - xfd->pollfd_n - 1) * sizeof(struct pollfd);
	//hlog(LOG_DEBUG, "xpoll_remove fd %d pollfd_n %d sizeof %d: 0x%x -> 0x%x len %d", xfd->fd, xfd->pollfd_n, sizeof(struct pollfd), (unsigned long)from, (unsigned long)to, len);
	memmove(to, from, len);
    }
	/* reduce pollfd_n for fds which where moved */
	struct xpoll_fd_t *xf;
	for (xf = xp->fds; (xf); xf = xf->next)
		if (xf->pollfd_n > xfd->pollfd_n) {
			xf->pollfd_n--;
			//hlog(LOG_DEBUG, "  ... fd %d is now pollfd_n %d", xf->fd, xf->pollfd_n);
		}
	xp->pollfd_used--;
#endif

	*xfd->prevp = xfd->next;
	if (xfd->next)
		xfd->next->prevp = xfd->prevp;
		
#ifndef _FOR_VALGRIND_
	cellfree( xpoll_fd_pool, xfd );
#else
	hfree(xfd);
#endif

	return 0;
}

/*
 *	enable / disable polling for writeability of a socket
 */

void xpoll_outgoing(struct xpoll_t *xp, struct xpoll_fd_t *xfd, int have_outgoing)
{
#ifdef XP_USE_POLL
	if (have_outgoing)
		xp->pollfd[xfd->pollfd_n].events |= POLLOUT;
	else
		xp->pollfd[xfd->pollfd_n].events &= POLLIN|POLLPRI|POLLERR|POLLHUP|POLLNVAL;
#endif
}

/*
 *	do a poll
 */

int xpoll(struct xpoll_t *xp, int timeout)
{
	int r;
	
#ifdef XP_USE_POLL
	r = poll(xp->pollfd, xp->pollfd_used, timeout);
	//hlog(LOG_DEBUG, "poll() returned %d", r);
	if (r <= 0)
		return r;
	
	struct xpoll_fd_t *xfd, *next;
	for (xfd = xp->fds; xfd; xfd = next) {
		next = xfd->next; /* the current one might be deleted by a handler */
		//hlog(LOG_DEBUG, "... checking fd %d", xfd->fd);
		xfd->result = 0;
		if (xp->pollfd[xfd->pollfd_n].revents) {
			if (xp->pollfd[xfd->pollfd_n].revents & (POLLIN|POLLPRI))
				xfd->result |= XP_IN;
			if (xp->pollfd[xfd->pollfd_n].revents & POLLOUT)
				xfd->result |= XP_OUT;
			if (xp->pollfd[xfd->pollfd_n].revents & (POLLERR|POLLHUP|POLLNVAL))
				xfd->result |= XP_ERR;
			(*xp->handler)(xp, xfd);
		}
	}
#endif
	
	return r;
}

