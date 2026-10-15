
/*
 *	Code to support SCTP connections
 */

#include "config.h"

#ifdef USE_SCTP

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <netinet/sctp.h>

#include "hmalloc.h"
#include "hlog.h"
#include "worker.h"
#include "sctp.h"

/* The socket send buffer. A client which does not keep up is disconnected
 * when the buffer is full, since the SCTP messages are not buffered in obuf.
 * Each queued message is charged with the kernel's overhead of up to a
 * kilobyte, so the default buffer of net.core.wmem_default holds only a
 * second or two of a full feed in small messages. Linux doubles the
 * request, to 512 kB, which holds at most some 15 seconds of a full feed of
 * 200 packets per second in the most efficiently packed messages, and caps
 * it at twice net.core.wmem_max (416 kB by default).
 */
#define SCTP_SNDBUF_SIZE (256*1024)

/* The interval of heartbeats on each path of a multi-homed association, in
 * milliseconds. The heartbeats find out whether the alternate paths work,
 * before the traffic has to move to one of them. 30 s by default.
 */
#define SCTP_HB_INTERVAL 15000

/* The value of SCTP_EXPOSE_POTENTIALLY_FAILED_STATE which enables the
 * notifications, the same as the sysctl net.sctp.pf_expose takes.
 */
#define SCTP_PF_EXPOSE_ENABLE 2

/*
 *	Select the notifications we want, and the sender dry event if
 *	sender_dry is set
 */

static int sctp_subscribe_events(struct client_t *c, int sender_dry)
{
	struct sctp_event_subscribe subscribe;
	
	memset(&subscribe, 0, sizeof(subscribe));
	
	subscribe.sctp_shutdown_event = 1;
	subscribe.sctp_association_event = 1;
	subscribe.sctp_address_event = 1;
	subscribe.sctp_send_failure_event = 1;
	subscribe.sctp_peer_error_event = 1;
	subscribe.sctp_partial_delivery_event = 1;
	subscribe.sctp_sender_dry_event = sender_dry;
	
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_EVENTS, (char *)&subscribe, sizeof(subscribe)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_EVENTS): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	return 0;
}

/*
 *	Failover between the paths of a multi-homed association. These are not
 *	fatal, the defaults work too.
 */

static void sctp_set_failover_params(struct client_t *c)
{
	/* heartbeat interval, for all the paths of the association */
	struct sctp_paddrparams pp;
	
	memset(&pp, 0, sizeof(pp));
	pp.spp_hbinterval = SCTP_HB_INTERVAL;
	pp.spp_flags = SPP_HB_ENABLE;
	
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, (char *)&pp, sizeof(pp)) == -1)
		hlog(LOG_WARNING, "setsockopt(%s, SCTP_PEER_ADDR_PARAMS): %s", c->addr_rem, strerror(errno));
	
#ifdef SCTP_PEER_ADDR_THLDS
	/* Move the traffic to another path after the first retransmission
	 * timeout on a path, when it becomes potentially failed (RFC 7829).
	 * It is the default of Linux (net.sctp.pf_retrans), but do not depend
	 * on the host's setting. A zero spt_pathmaxrxt leaves it unchanged.
	 */
	struct sctp_paddrthlds th;
	
	memset(&th, 0, sizeof(th));
	th.spt_pathpfthld = 0;
	
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_PEER_ADDR_THLDS, (char *)&th, sizeof(th)) == -1)
		hlog(LOG_WARNING, "setsockopt(%s, SCTP_PEER_ADDR_THLDS): %s", c->addr_rem, strerror(errno));
#endif

#ifdef SCTP_EXPOSE_POTENTIALLY_FAILED_STATE
	/* Ask to be notified of a path becoming potentially failed, and
	 * available again, so that a failover gets logged. The kernel's
	 * default (sysctl net.sctp.pf_expose 0) leaves it to the socket.
	 * The values are those of the sysctl: 0 unset, 1 disabled,
	 * 2 enabled; the enum is not in the public kernel headers.
	 */
	struct sctp_assoc_value av;

	memset(&av, 0, sizeof(av));
	av.assoc_value = SCTP_PF_EXPOSE_ENABLE;

	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_EXPOSE_POTENTIALLY_FAILED_STATE, (char *)&av, sizeof(av)) == -1)
		hlog(LOG_WARNING, "setsockopt(%s, SCTP_EXPOSE_POTENTIALLY_FAILED_STATE): %s", c->addr_rem, strerror(errno));
#endif
}

int sctp_set_client_sockopt(struct client_t *c)
{
	int sndbuf = SCTP_SNDBUF_SIZE;
	if (setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == -1) {
		/* not fatal, the default buffer works too */
		hlog(LOG_WARNING, "setsockopt(%s, SO_SNDBUF, %d): %s", c->addr_rem, sndbuf, strerror(errno));
	}
	
	int enable = 1;
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_NODELAY, &enable, sizeof(enable)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_NODELAY): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	sctp_set_failover_params(c);
	
	return sctp_subscribe_events(c, 0);
}

/*
 *	Set the socket options which take effect after the login: send the
 *	messages unordered from now on. The messages of the login handshake
 *	are sent in order, since a retransmitted message could otherwise be
 *	delivered after the ones sent after it.
 */

int sctp_set_client_sockopt_post_login(struct client_t *c)
{
	struct sctp_sndrcvinfo sri;
	socklen_t len;
	
	/* default sendmsg() parameters */
	len = sizeof(sri);
	if (getsockopt(c->fd, IPPROTO_SCTP, SCTP_DEFAULT_SEND_PARAM, (char *)&sri, &len) == -1) {
		hlog(LOG_ERR, "getsockopt(%s, SCTP_DEFAULT_SEND_PARAM): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	sri.sinfo_flags = SCTP_UNORDERED;
	
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_DEFAULT_SEND_PARAM, (char *)&sri, len) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_DEFAULT_SEND_PARAM): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	return 0;
}

/*
 *	After the login, ask for the sender dry event, which arrives when the
 *	peer has acknowledged all the data sent so far, or right away if it
 *	already has. Returns -1 if the event can not be had, and the data can
 *	be started at once.
 */

int sctp_wait_sender_dry(struct client_t *c)
{
	if (sctp_subscribe_events(c, 1))
		return -1;
	
	c->sctp_dry_wait = 1;
	
	return 0;
}

/*
 *	The peer has acknowledged the login handshake, start sending data.
 *	The sender dry event is not needed any more: it would arrive every
 *	time the peer has caught up.
 */

static void sctp_rx_sender_dry(struct worker_t *self, struct client_t *c)
{
	if (!c->sctp_dry_wait)
		return;
	
	hlog(LOG_DEBUG, "%s/%s: SCTP login acknowledged, starting data", c->addr_rem, c->username);
	
	c->sctp_dry_wait = 0;
	sctp_subscribe_events(c, 0);
	worker_client_data_start(self, c);
}

/*
 *	Set parameters for a listener socket
 */

int sctp_set_listen_params(struct listen_t *l)
{
	struct sctp_event_subscribe subscribe;
	
	memset(&subscribe, 0, sizeof(subscribe));
	
	subscribe.sctp_data_io_event = 1;
	subscribe.sctp_association_event = 1;
	
	if (setsockopt(l->fd, IPPROTO_SCTP, SCTP_EVENTS, (char *)&subscribe, sizeof(subscribe)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_EVENTS): %s", l->addr_s, strerror(errno));
		return -1;
	}
	
	return l->fd;
}

/*
 *	SCTP notification received
 */

static int sctp_rx_assoc_change(struct client_t *c, union sctp_notification *sn)
{
	switch (sn->sn_assoc_change.sac_state) {
	case SCTP_COMM_UP:
		hlog(LOG_INFO, "%s/%s: SCTP COMM_UP - connection established", c->addr_rem, c->username);
		break;
	case SCTP_COMM_LOST:
		hlog(LOG_INFO, "%s/%s: SCTP COMM_LOST - connection failed", c->addr_rem, c->username);
		break;
	case SCTP_RESTART:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_RESTART", c->addr_rem, c->username);
		break;
	case SCTP_SHUTDOWN_COMP:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_SHUTDOWN_COMP", c->addr_rem, c->username);
		break;
	case SCTP_CANT_STR_ASSOC:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_CANT_STR_ASSOC", c->addr_rem, c->username);
		break;
	default:
		hlog(LOG_DEBUG, "%s/%s: SCTP Received unexpected assoc_change %d", c->addr_rem, c->username, sn->sn_assoc_change.sac_state);
		break;
	}
	
	if (sn->sn_assoc_change.sac_state == SCTP_COMM_UP)
		return sn->sn_assoc_change.sac_assoc_id;
	
	return 0;
	
}

static int sctp_rx_peer_addr_change(struct client_t *c, union sctp_notification *sn)
{
	char *addr_s = strsockaddr((struct sockaddr *)&sn->sn_paddr_change.spc_aaddr, sizeof(sn->sn_paddr_change.spc_aaddr));
	
	switch (sn->sn_paddr_change.spc_state) {
	case SCTP_ADDR_AVAILABLE:
		hlog(LOG_INFO, "%s/%s: SCTP peer address available: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_UNREACHABLE:
		hlog(LOG_INFO, "%s/%s: SCTP peer address unreachable: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_REMOVED:
		hlog(LOG_INFO, "%s/%s: SCTP peer address removed: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_ADDED:
		hlog(LOG_INFO, "%s/%s: SCTP peer address added: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_MADE_PRIM:
		hlog(LOG_INFO, "%s/%s: SCTP peer address made primary: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_CONFIRMED:
		/* each address of a multi-homed peer, when the association is set up */
		hlog(LOG_DEBUG, "%s/%s: SCTP peer address confirmed: %s", c->addr_rem, c->username, addr_s);
		break;
#ifdef SCTP_ADDR_PF
	case SCTP_ADDR_PF:
		/* enabled in sctp_set_failover_params(), or with sysctl net.sctp.pf_expose = 2 */
		hlog(LOG_INFO, "%s/%s: SCTP peer address potentially failed, traffic moves to another address if one is available: %s", c->addr_rem, c->username, addr_s);
		break;
#endif
	default:
		hlog(LOG_DEBUG, "%s/%s: SCTP Received unexpected peer_addr_change %d: %s",
			c->addr_rem, c->username, sn->sn_paddr_change.spc_state, addr_s);
		break;
	}
	
	hfree(addr_s);
	
	return 0;
	
}

static int sctp_rx_notification(struct worker_t *self, struct client_t *c, struct msghdr *m)
{
	union sctp_notification *sn;
	
	sn = (union sctp_notification *)m->msg_iov->iov_base;
	
	switch(sn->sn_header.sn_type) {
	case SCTP_SHUTDOWN_EVENT: {
		struct sctp_shutdown_event *shut;
		shut = (struct sctp_shutdown_event *)m->msg_iov->iov_base; 
		hlog(LOG_INFO, "%s/%s: SCTP shutdown on assoc id %d", c->addr_rem, c->username, shut->sse_assoc_id); 
		return -1;
	}
	case SCTP_ASSOC_CHANGE:
		return sctp_rx_assoc_change(c, sn);
	case SCTP_PEER_ADDR_CHANGE:
		return sctp_rx_peer_addr_change(c, sn);
	case SCTP_SEND_FAILED:
		hlog(LOG_DEBUG, "%s/%s: SCTP send failed", c->addr_rem, c->username);
		return 0;
	case SCTP_REMOTE_ERROR:
		hlog(LOG_DEBUG, "%s/%s: SCTP remote error", c->addr_rem, c->username);
		return 0;
	case SCTP_PARTIAL_DELIVERY_EVENT:
		hlog(LOG_DEBUG, "%s/%s: SCTP partial delivery event", c->addr_rem, c->username);
		return 0;
	case SCTP_ADAPTATION_INDICATION:
		hlog(LOG_DEBUG, "%s/%s: SCTP adaptation indication", c->addr_rem, c->username);
		return 0;
	case SCTP_AUTHENTICATION_INDICATION:
		hlog(LOG_DEBUG, "%s/%s: SCTP authentication indication", c->addr_rem, c->username);
		return 0;
	case SCTP_SENDER_DRY_EVENT:
		sctp_rx_sender_dry(self, c);
		return 0;
	default:
		hlog(LOG_ERR, "%s/%s: sctp_rx_notification: Received unexpected notification: %d",
			c->addr_rem, c->username, sn->sn_header.sn_type);
	};

	return -1;
}

typedef union {
	struct sctp_initmsg init;
	struct sctp_sndrcvinfo sndrcvinfo;
} _sctp_cmsg_data_t;

typedef union {
	struct sockaddr_storage ss;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
	struct sockaddr sa;
} sockaddr_storage_t;

/*
 *	handle a readable event on SCTP socket
 */

int sctp_readable(struct worker_t *self, struct client_t *c)
{
	int e;
	struct msghdr inmsg;
	char incmsg[CMSG_SPACE(sizeof(_sctp_cmsg_data_t))];
	sockaddr_storage_t msgname;
	struct iovec iov;
	
	/* space to receive data */
	c->ibuf_end = 0;
	iov.iov_base = c->ibuf;
	iov.iov_len = c->ibuf_size - 3;
	inmsg.msg_flags = 0;
	inmsg.msg_iov = &iov;
	inmsg.msg_iovlen = 1;
	/* or control messages */
	inmsg.msg_control = incmsg;
	inmsg.msg_controllen = sizeof(incmsg);
	inmsg.msg_name = &msgname;
	inmsg.msg_namelen = sizeof(msgname);
	
	e = recvmsg(c->fd, &inmsg, MSG_WAITALL);
	if (e < 0) {
		if (errno == EAGAIN) {
			hlog(LOG_DEBUG, "sctp_readable: EAGAIN");
			return 0;
		}
		
		hlog(LOG_INFO, "%s/%s: sctp_readable: recvmsg returned %d: %s", c->addr_rem, c->username, e, strerror(errno));
		client_close(self, c, errno);
		return -1;
	}
	
	if (e == 0) {
		hlog(LOG_DEBUG, "%s/%s: sctp_readable: EOF from socket fd %d",
		      c->addr_rem, c->username, c->fd);
		client_close(self, c, CLIERR_EOF);
		return -1;
	}
	
	if (inmsg.msg_flags & MSG_NOTIFICATION) {
		//hlog(LOG_DEBUG, "%s/%s: sctp_readable: got MSG_NOTIFICATION", c->addr_rem, c->username);
		sctp_rx_notification(self, c, &inmsg);
		return 0;
	}
	
	//hlog_packet(LOG_DEBUG, iov.iov_base, e, "sctp_readable: got data: ");
	
	return client_postread(self, c, e);
}

/*
 *	SCTP socket is now writable, flush the data buffered in obuf
 */

int sctp_writable(struct worker_t *self, struct client_t *c)
{
	if (sctp_client_write(self, c, NULL, 0) < 0)
		return -1;
	
	return 0;
}

/*
 *	Write data to an SCTP client
 */

int sctp_client_write(struct worker_t *self, struct client_t *c, char *p, int len)
{
	//hlog_packet(LOG_DEBUG, p, len, "%s/%s: client_write_sctp %d bytes: ", c->addr_rem, c->username, len);
	
	if (len > 0) {
		c->obuf_writes++;
		if (client_buffer_outgoing_data(self, c, p, len) == -12)
			return -12;
		clientaccount_add_tx(c, IPPROTO_SCTP, len, 0);
		if (c->obuf_writes > obuf_writes_threshold) {
			// Lots and lots of writes, switch to buffering...
			if (c->obuf_flushsize == 0) {
				c->obuf_flushsize = c->obuf_size / 2;
			}
		}
	}

	/* Is it over the flush size ? */
	if (c->obuf_end > c->obuf_flushsize || ((len == 0) && (c->obuf_end > c->obuf_start))) {
		int to_send = c->obuf_end - c->obuf_start;
		int i = send(c->fd, c->obuf + c->obuf_start, to_send, 0);
		
		if (i < 0) {
			hlog(LOG_ERR, "%s/%s: SCTP transmit error to fd %d: %s",
				c->addr_rem, c->username, c->fd, strerror(errno));
			client_close(self, c, errno);
			return -9;
		} else if (i != to_send) {
			// Incomplete write with SCTP is not great, as we might not have ordered delivery.
			hlog(LOG_ERR, "%s/%s: SCTP transmit incomplete to fd %d: wrote %d of %d bytes, errno: %s",
				c->addr_rem, c->username, c->fd, i, to_send, strerror(errno));
			client_close(self, c, errno);
			return -9;
		} else {
			//hlog(LOG_DEBUG, "%s/%s: SCTP transmit ok: %d bytes", c->addr_rem, c->username, i);
		}
		c->obuf_start += i;
		c->obuf_wtime = tick;
	}

	/* All done ? */
	if (c->obuf_start >= c->obuf_end) {
		//hlog(LOG_DEBUG, "%s/%s: client_write obuf empty", c->addr_rem, c->username, c->addr_rem);
		c->obuf_start = 0;
		c->obuf_end   = 0;
		return len;
	}
	
	/* tell the poller that we have outgoing data, to flush it at the
	 * end of this round instead of waiting for the next keepalive scan
	 */
	xpoll_outgoing(&self->xp, c->xfd, 1);
	
	return len;
}

#endif
