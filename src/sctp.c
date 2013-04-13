
#include "config.h"
#include "hlog.h"
#include "sctp.h"
#include "worker.h"

/*
 *	Code to support SCTP connections
 */

#ifdef USE_SCTP

#include <netinet/sctp.h>

/*
 *	SCTP notification received
 */

static int sctp_rx_assoc_change(struct client_t *c, union sctp_notification *sn)
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
	};
	
	hlog(LOG_ERR, "sctp_rx_notification: Received unexpected notification: %d", sn->sn_header.sn_type);
	
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
		int associd = sctp_rx_notification(c, &inmsg);
		return 0;
	}
	
	//hlog_packet(LOG_DEBUG, iov.iov_base, e, "sctp_readable: got data: ");
	c->ibuf[e++] = '\r';
	c->ibuf[e++] = '\n';
	
	return client_postread(self, c, e);
}

/*
 *	SCTP socket is now writeable, but we really don't do SCTP buffering yet...
 */

int sctp_writeable(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_INFO, "sctp_writeable: SCTP tx buffering not implemented, closing socket");
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

#endif
