
#include "config.h"
#include "hmalloc.h"
#include "hlog.h"
#include "sctp.h"
#include "worker.h"

/*
 *	Code to support SCTP connections
 */

#ifdef USE_SCTP

#include <netinet/sctp.h>

int sctp_set_client_sockopt(struct client_t *c)
{
	struct sctp_sndrcvinfo sri;
	socklen_t len;
	
	/* default sendmsg() parameters */
	len = sizeof(sri);
	if (getsockopt(c->fd, SOL_SCTP, SCTP_DEFAULT_SEND_PARAM, (char *)&sri, &len) == -1) {
		hlog(LOG_ERR, "getsockopt(%s, SCTP_DEFAULT_SEND_PARAM): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	sri.sinfo_flags = SCTP_UNORDERED;
	
	if (setsockopt(c->fd, SOL_SCTP, SCTP_DEFAULT_SEND_PARAM, (char *)&sri, len) == -1) {
		hlog(LOG_ERR, "setsockopt(%s, SCTP_DEFAULT_SEND_PARAM): %s", c->addr_rem, strerror(errno));
		return -1;
	}
	
	/* which notifications do we want? */
	struct sctp_event_subscribe subscribe;
	
	memset(&subscribe, 0, sizeof(subscribe));
	
	subscribe.sctp_association_event = 1;
	subscribe.sctp_address_event = 1;
	subscribe.sctp_send_failure_event = 1;
	subscribe.sctp_peer_error_event = 1;
	subscribe.sctp_partial_delivery_event = 1;
	
	if (setsockopt(c->fd, SOL_SCTP, SCTP_EVENTS, (char *)&subscribe, sizeof(subscribe)) == -1) {
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
	
	if (setsockopt(l->fd, SOL_SCTP, SCTP_EVENTS, (char *)&subscribe, sizeof(subscribe)) == -1) {
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
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_COMM_UP", c->addr_rem, c->username);
		break;
	case SCTP_COMM_LOST:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_COMM_LOST", c->addr_rem, c->username);
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
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_ADDR_AVAILABLE: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_UNREACHABLE:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_ADDR_UNREACHABLE: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_REMOVED:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_ADDR_REMOVED: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_ADDED:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_ADDR_ADDED: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_MADE_PRIM:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_ADDR_MADE_PRIM: %s", c->addr_rem, c->username, addr_s);
		break;
	case SCTP_ADDR_CONFIRMED:
		hlog(LOG_DEBUG, "%s/%s: Received SCTP_ADDR_CONFIRMED: %s", c->addr_rem, c->username, addr_s);
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
		hlog(LOG_INFO, "SCTP shutdown on assoc id %d",  shut->sse_assoc_id); 
		break; 
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
	};
	
	hlog(LOG_ERR, "%s/%s: sctp_rx_notification: Received unexpected notification: %d",
		c->addr_rem, c->username, sn->sn_header.sn_type);
	
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
		
		hlog(LOG_INFO, "sctp_readable: recvmsg returned %d: %s", e, strerror(errno));
		
		client_close(self, c, errno);
		return -1;
	}
	
	if (e == 0) {
		hlog( LOG_DEBUG, "sctp_readable: EOF from socket fd %d (%s @ %s)",
		      c->fd, c->addr_rem, c->addr_loc );
		client_close(self, c, CLIERR_EOF);
		return -1;
	}
	
	if (inmsg.msg_flags & MSG_NOTIFICATION) {
		hlog(LOG_DEBUG, "sctp_readable: got MSG_NOTIFICATION");
		sctp_rx_notification(c, &inmsg);
		return 0;
	}
	
	//hlog_packet(LOG_DEBUG, iov.iov_base, e, "sctp_readable: got data: ");
	c->ibuf[e++] = '\r';
	c->ibuf[e++] = '\n';
	
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
	//hlog_packet(LOG_DEBUG, p, len, "client_write_sctp %d bytes: ", len);
	
	if (len == 0)
		return 0;
		
	clientaccount_add( c, IPPROTO_SCTP, 0, 0, len, 0, 0, 0);
	
	int i = send(c->fd, p, len-2, 0);
	
	if (i < 0) {
		hlog(LOG_ERR, "SCTP transmit error to fd %d / %s: %s",
			c->fd, c->addr_rem, strerror(errno));
	} else if (i != len -2) {
		hlog(LOG_ERR, "SCTP transmit incomplete to fd %d / %s: wrote %d of %d bytes, errno: %s",
			c->fd, c->addr_rem, i, len-2, strerror(errno));
	} else {
		//hlog(LOG_DEBUG, "SCTP transmit ok to %s: %d bytes", c->addr_rem, i);
		c->obuf_wtime = tick;
	}
	
	return i;
}


#if 0


/*
 *	SCTP notification received
 */

static int sctp_rx_assoc_change(struct listen_t *l, union sctp_notification *sn)
{
	switch (sn->sn_assoc_change.sac_state) {
	case SCTP_COMM_UP:
		hlog(LOG_DEBUG, "Received SCTP_COMM_UP");
		break;
	case SCTP_COMM_LOST:
		hlog(LOG_DEBUG, "Received SCTP_COMM_LOST");
		break;
	case SCTP_RESTART:
		hlog(LOG_DEBUG, "Received SCTP_RESTART");
		break;
	case SCTP_SHUTDOWN_COMP:
		hlog(LOG_DEBUG, "Received SCTP_SHUTDOWN_COMP");
		break;
	case SCTP_CANT_STR_ASSOC:
		hlog(LOG_DEBUG, "Received SCTP_CANT_STR_ASSOC");
		break;
	default:
		hlog(LOG_DEBUG, "Received assoc_change %d", sn->sn_assoc_change.sac_state);
		break;
	}
	
	if (sn->sn_assoc_change.sac_state == SCTP_COMM_UP)
		return sn->sn_assoc_change.sac_assoc_id;
	
	return 0;
	
}

static int sctp_rx_notification(struct listen_t *l, struct msghdr *m)
{
	union sctp_notification *sn;
	
	sn = (union sctp_notification *)m->msg_iov->iov_base;
	
	switch(sn->sn_header.sn_type) {
	case SCTP_SHUTDOWN_EVENT: {
		struct sctp_shutdown_event *shut;
		shut = (struct sctp_shutdown_event *)m->msg_iov->iov_base; 
		hlog(LOG_INFO, "SCTP shutdown on assoc id %d",  shut->sse_assoc_id); 
		break; 
	}
	case SCTP_ASSOC_CHANGE:
		return sctp_rx_assoc_change(l, sn);
	};
	
	hlog(LOG_ERR, "sctp_rx_notification: Received unexpected notification: %d", sn->sn_header.sn_type);
	
	return -1;
}

/*
 *	Receive something on an SCTP socket
 */


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

static void accept_sctp(struct listen_t *l)
{
	int e;
	struct msghdr inmsg;
	char incmsg[CMSG_SPACE(sizeof(_sctp_cmsg_data_t))];
	sockaddr_storage_t msgname;
	struct iovec iov;
	char buf[2000];
	
	/* space to receive data */
	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	inmsg.msg_flags = 0;
	inmsg.msg_iov = &iov;
	inmsg.msg_iovlen = 1;
	/* or control messages */
	inmsg.msg_control = incmsg;
	inmsg.msg_controllen = sizeof(incmsg);
	inmsg.msg_name = &msgname;
	inmsg.msg_namelen = sizeof(msgname);
	
	e = recvmsg(l->fd, &inmsg, MSG_WAITALL);
	if (e < 0) {
		if (errno == EAGAIN) {
			hlog(LOG_DEBUG, "accept_sctp: EAGAIN");
			return;
		}
		
		hlog(LOG_INFO, "accept_sctp: recvmsg returned %d: %s", e, strerror(errno));
	}
	
	if (inmsg.msg_flags & MSG_NOTIFICATION) {
		hlog(LOG_DEBUG, "accept_sctp: got MSG_NOTIFICATION");
		int associd = sctp_rx_notification(l, &inmsg);
	} else {
		hlog_packet(LOG_DEBUG, iov.iov_base, e, "accept_sctp: got data: ");
	}
	
}
#endif


#endif
