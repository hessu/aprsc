
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

int sctp_set_client_sockopt(struct client_t *c)
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

	int enable = 1;
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_NODELAY, &enable, sizeof(enable)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_NODELAY): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	/* which notifications do we want? */
	struct sctp_event_subscribe subscribe;
	
	memset(&subscribe, 0, sizeof(subscribe));
	
	subscribe.sctp_shutdown_event = 1;
	subscribe.sctp_association_event = 1;
	subscribe.sctp_address_event = 1;
	subscribe.sctp_send_failure_event = 1;
	subscribe.sctp_peer_error_event = 1;
	subscribe.sctp_partial_delivery_event = 1;
	
	if (setsockopt(c->fd, IPPROTO_SCTP, SCTP_EVENTS, (char *)&subscribe, sizeof(subscribe)) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_EVENTS): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	return 0;
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
		hlog(LOG_DEBUG, "%s/%s: SCTP peer address available: %s", c->addr_rem, c->username, addr_s);
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
		hlog(LOG_INFO, "%s/%s: SCTP peer address confirmed: %s", c->addr_rem, c->username, addr_s);
		break;
	default:
		hlog(LOG_DEBUG, "%s/%s: SCTP Received unexpected peer_addr_change %d: %s",
			c->addr_rem, c->username, sn->sn_assoc_change.sac_state, addr_s);
		break;
	}
	
	hfree(addr_s);
	
	return 0;
	
}

static int sctp_rx_notification(struct client_t *c, struct msghdr *m)
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
		hlog(LOG_DEBUG, "%s/%s: SCTP sender dry", c->addr_rem, c->username);
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
		sctp_rx_notification(c, &inmsg);
		return 0;
	}
	
	//hlog_packet(LOG_DEBUG, iov.iov_base, e, "sctp_readable: got data: ");
	
	return client_postread(self, c, e);
}

/*
 *	SCTP socket is now writable, but we really don't do SCTP buffering yet...
 */

int sctp_writable(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_INFO, "sctp_writable: SCTP tx buffering not implemented, closing socket");
	client_close(self, c, errno);
	return -1;
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
	}
	return len;
}

#endif
