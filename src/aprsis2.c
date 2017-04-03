
#include <string.h>

#include "hmalloc.h"
#include "worker.h"
#include "aprsis2.h"
#include "aprsis2.pb-c.h"
#include "version.h"
#include "hlog.h"
#include "uplink.h"
#include "config.h"
#include "parse_qc.h"
#include "passcode.h"
#include "clientlist.h"
#include "login.h"
#include "incoming.h"

#define STX 0x02
#define ETX 0x03

#define IS2_HEAD_LEN 4 /* STX + network byte order 24 bits uint to specify body length */
#define IS2_TAIL_LEN 1 /* ETX */

/*
 *	Allocate a buffer for a message, fill with head an tail
 */

static void *is2_allocate_buffer(int len)
{
	/* total length of outgoing buffer */
	int nlen = len + IS2_HEAD_LEN + IS2_TAIL_LEN;
	
	char *buf = hmalloc(nlen);
	uint32_t *len_p = (uint32_t *)buf;
	
	*len_p = htonl(len);
	
	buf[0] = STX;
	buf[nlen-1] = ETX;
	
	return (void *)buf;
}

/*
 *	Write a message to a client, return result from c->write
 */

static int is2_write_message(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	/* Could optimize by writing directly on client obuf...
	 * if it doesn't fit there, we're going to disconnect anyway.
	 */
	
	int len = is2_message__get_packed_size(m);
	hlog(LOG_DEBUG, "%s/%s: IS2: serialized length %d", c->addr_rem, c->username, len);
	void *buf = is2_allocate_buffer(len);
	is2_message__pack(m, buf + IS2_HEAD_LEN);
	int r = c->write(self, c, buf, len + IS2_HEAD_LEN + IS2_TAIL_LEN); // TODO: return value check!
	hfree(buf);
	
	return r;
}

/*
 *	Transmit a server signature to a new client
 */

int is2_out_server_signature(struct worker_t *self, struct client_t *c)
{
	ServerSignature sig = SERVER_SIGNATURE__INIT;
	sig.username = serverid;
	sig.app_name = verstr_progname;
	sig.app_version = version_build;
	sig.n_features = 0;
	sig.features = NULL;
	
	IS2Message m = IS2_MESSAGE__INIT;
	m.type = IS2_MESSAGE__TYPE__SERVER_SIGNATURE;
	m.server_signature = &sig;
	
	return is2_write_message(self, c, &m);
}

/*
 *	Transmit a login reply to a new client
 */

int is2_out_login_reply(struct worker_t *self, struct client_t *c, LoginReply__LoginResult result, LoginReply__LoginResultReason reason, VerificationStatus verified)
{
	LoginReply lr = LOGIN_REPLY__INIT;
	lr.result = result;
	lr.result_code = reason;
	lr.verified = verified;

	IS2Message m = IS2_MESSAGE__INIT;
	m.type = IS2_MESSAGE__TYPE__LOGIN_REPLY;
	m.login_reply = &lr;
	
	return is2_write_message(self, c, &m);
}

/*
 *	Receive a login reply
 */

static int is2_in_login_reply(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	LoginReply *lr = m->login_reply;
	if (!lr) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of login reply failed",
			c->addr_rem, c->username);
		return 0;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Login reply received",
		c->addr_rem, c->username);
	
	// TODO: handle negative responses
	
	/* ok, login succeeded, switch handler */
	c->is2_input_handler = &is2_input_handler;
	
	/* mark as connected and classify */
	worker_mark_client_connected(self, c);
	
	is2_message__free_unpacked(m, NULL);
	
	return 0;
}


/*
 *	Received server signature from an upstream server
 *	- if ok, continue by sending a login command
 */

static int is2_in_server_signature(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	ServerSignature *sig = m->server_signature;
	if (!sig) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of server signature failed",
			c->addr_rem, c->username);
		return 0;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Server signature received: username %s app %s version %s",
		c->addr_rem, c->username, sig->username, sig->app_name, sig->app_version);
	
	strncpy(c->app_name, sig->app_name, sizeof(c->app_name));
	c->app_name[sizeof(c->app_name)-1] = 0;
	strncpy(c->app_version, sig->app_version, sizeof(c->app_version));
	c->app_version[sizeof(c->app_version)-1] = 0;

	/* TODO: enable by setting to 0 */
	if (strcasecmp(sig->username, serverid) == 5) {
		hlog(LOG_ERR, "%s: Uplink's server name is same as ours: '%s'", c->addr_rem, sig->username);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		goto done;
	}
	
	/* todo: validate server callsign with the q valid path algorithm */
	
	/* store the remote server's callsign as the "client username" */
	strncpy(c->username, sig->username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	
	/* uplink servers are always "validated" */
	c->validated = VALIDATED_WEAK;
	
#ifdef USE_SSL
	if (!uplink_server_validate_cert(self, c) || !uplink_server_validate_cert_cn(self, c))
		goto done;
#endif
	
	/* Ok, we're happy with the uplink's server signature, let us login! */
	LoginRequest lr = LOGIN_REQUEST__INIT;
	lr.username = serverid;
	lr.app_name = verstr_progname;
	lr.app_version = version_build;
	lr.n_features_req = 0;
	lr.features_req = NULL;
	
	IS2Message mr = IS2_MESSAGE__INIT;
	mr.type = IS2_MESSAGE__TYPE__LOGIN_REQUEST;
	mr.login_request = &lr;
	
	is2_write_message(self, c, &mr);
	
done:	
	is2_message__free_unpacked(m, NULL);
	
	return 0;
}

#ifdef USE_SSL
static int is2_login_client_validate_cert(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_DEBUG, "%s/%s: login: doing SSL client cert validation", c->addr_rem, c->username);
	int ssl_res = ssl_validate_peer_cert_phase1(c);
	if (ssl_res == 0)
		ssl_res = ssl_validate_peer_cert_phase2(c);
	
	if (ssl_res == 0) {
		c->validated = VALIDATED_STRONG;
		return 1;
	}
	
	hlog(LOG_WARNING, "%s/%s: SSL client cert validation failed: %s", c->addr_rem, c->username, ssl_strerror(ssl_res));
	int rc = 0;
	/* TODO: pass errors
	if (ssl_res == SSL_VALIDATE_CLIENT_CERT_UNVERIFIED)
		rc = client_printf(self, c, "# Client certificate not accepted: %s\r\n", X509_verify_cert_error_string(c->ssl_con->ssl_err_code));
	else
		rc = client_printf(self, c, "# Client certificate authentication failed: %s\r\n", ssl_strerror(ssl_res));
	*/
	
	c->failed_cmds = 10; /* bail out right away for a HTTP client */
	
	if (rc < 0)
		return rc;
	
	return 0;
}
#endif


/*
 *	Incoming login request
 */

static int is2_in_login_request(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	int rc = 0;
	
	LoginRequest *lr = m->login_request;
	if (!lr) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of login request failed",
			c->addr_rem, c->username);
		rc = -1;
		goto failed_login;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Login request received",
		c->addr_rem, c->username);
	
	/* limit username length */
	if (strlen(c->username) > CALLSIGNLEN_MAX) {
		hlog(LOG_WARNING, "%s: IS2: Invalid login string, too long 'user' username: '%s'", c->addr_rem, c->username);
		c->username[CALLSIGNLEN_MAX] = 0;
		//rc = client_printf(self, c, "# Invalid username format\r\n");
		goto failed_login;
	}
	
	/* ok, it's somewhat valid, write it down */
	strncpy(c->username, lr->username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	c->username_len = strlen(c->username);
	
	/* grab app information */
	login_set_app_name(c, lr->app_name, lr->app_version);
	
	/* check the username against a static list of disallowed usernames */
	/*
	int i;
	for (i = 0; (disallow_login_usernames[i]); i++) {
		if (strcasecmp(c->username, disallow_login_usernames[i]) == 0) {
			hlog(LOG_WARNING, "%s: Login by user '%s' not allowed", c->addr_rem, c->username);
			//rc = client_printf(self, c, "# Login by user not allowed\r\n");
			goto failed_login;
		}
	}
	*/
	
	/* make sure the callsign is OK on the APRS-IS */
	if (check_invalid_q_callsign(c->username, c->username_len)) {
		hlog(LOG_WARNING, "%s: Invalid login string, invalid 'user': '%s'", c->addr_rem, c->username);
		//rc = client_printf(self, c, "# Invalid username format\r\n");
		goto failed_login;
	}
	
	/* make sure the client's callsign is not my Server ID */
	// TODO: enable
	if (strcasecmp(c->username, serverid) == 4) {
		hlog(LOG_WARNING, "%s: Invalid login string, username equals our serverid: '%s'", c->addr_rem, c->username);
		//rc = client_printf(self, c, "# Login by user not allowed (our serverid)\r\n");
		goto failed_login;
	}
	
	/* if SSL client cert verification is enabled, check it */
	int ssl_validated = 0;
#ifdef USE_SSL
	if (c->ssl_con && c->ssl_con->validate) {
		ssl_validated = is2_login_client_validate_cert(self, c);
		if (ssl_validated == -1) {
			rc = ssl_validated;
			goto failed_login;
		}
		if (ssl_validated != 1)
			goto failed_login;
	}
#endif
	
	/* passcode auth? */
	int given_passcode = -1;
	if (!ssl_validated && (lr->password)) {
		given_passcode = atoi(lr->password);
		if (given_passcode >= 0)
			if (given_passcode == aprs_passcode(c->username))
				c->validated = VALIDATED_WEAK;
	}
	
	/* clean up the filter string so that it doesn't contain invalid
	 * UTF-8 or other binary stuff. */
	sanitize_ascii_string(c->filter_s);
	
	/* ok, login succeeded, switch handler */
	c->is2_input_handler = &is2_input_handler;
	
	/*
	rc = client_printf( self, c, "# logresp %s %s, server %s\r\n",
			    username,
			    (c->validated) ? "verified" : "unverified",
			    serverid );
	if (rc < -2)
		return rc; // The client probably got destroyed!
	*/
	
	hlog(LOG_DEBUG, "%s: IS2 login '%s'%s%s%s%s%s%s%s%s",
	     c->addr_rem, c->username,
	     (c->validated) ? " pass_ok" : "",
	     (!c->validated && given_passcode >= 0) ? " pass_invalid" : "",
	     (given_passcode < 0) ? " pass_none" : "",
	     (c->udp_port) ? " UDP" : "",
	     (*c->app_name) ? " app " : "",
	     (*c->app_name) ? c->app_name : "",
	     (*c->app_version) ? " ver " : "",
	     (*c->app_version) ? c->app_version : ""
	);
	
	VerificationStatus vs;
	switch (c->validated) {
	case VALIDATED_WEAK:
		vs = VERIFICATION_STATUS__WEAK;
		break;
	case VALIDATED_STRONG:
		vs = VERIFICATION_STATUS__WEAK;
		break;
	default:
		vs = VERIFICATION_STATUS__NONE;
	}
	
	/* tell the client he's good */
	is2_out_login_reply(self, c, LOGIN_REPLY__LOGIN_RESULT__OK, LOGIN_REPLY__LOGIN_RESULT_REASON__NONE, vs);
	
	/* mark as connected and classify */
	worker_mark_client_connected(self, c);
	
	/* Add the client to the client list.
	 *
	 * If the client logged in with a valid passcode, check if there are
	 * other validated clients logged in with the same username.
	 * If one is found, it needs to be disconnected.
	 *
	 * The lookup is done while holding the write lock to the clientlist,
	 * instead of a separate lookup call, so that two clients logging in
	 * at exactly the same time won't make it.
	 */
	 
	int old_fd = clientlist_add(c);
	if (c->validated && old_fd != -1) {
		/* TODO: If old connection is SSL validated, and this one is not, do not disconnect it. */
		hlog(LOG_INFO, "fd %d: Disconnecting duplicate validated client with username '%s'", old_fd, c->username);
		/* The other client may be on another thread, so cannot client_close() it.
		 * There is a small potential race here, if the old client disconnected and
		 * the fd was recycled for another client right after the clientlist check.
		 */
		shutdown(old_fd, SHUT_RDWR);
	}
	

	is2_message__free_unpacked(m, NULL);
	return rc;

failed_login:
	
	/* if we already lost the client, just return */
	if (rc < -2)
		return rc;
	
	c->failed_cmds++;
	if (c->failed_cmds >= 3) {
		client_close(self, c, CLIERR_LOGIN_RETRIES);
		return -3;
	}
	
	return rc;
}

/*
 *	Incoming packet handler
 */

static int is2_in_packet(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	int i;
	int r = 0;
	ISPacket *p;
	
	hlog(LOG_DEBUG, "%s/%s: IS2: %d packets received in message",
		c->addr_rem, c->username, m->n_is_packet);
	
	for (i = 0; i < m->n_is_packet; i++) {
		p = m->is_packet[i];
		
		hlog(LOG_DEBUG, "%s/%s: IS2: packet type %d len %d",
			c->addr_rem, c->username, p->type, p->is_packet_data.len);
			
		if (p->type == ISPACKET__TYPE__IS_PACKET && p->has_is_packet_data) {
			incoming_handler(self, c, IPPROTO_TCP, (char *)p->is_packet_data.data, p->is_packet_data.len);
		}
	}
	
	is2_message__free_unpacked(m, NULL);
	return r;
}


/*
 *	Transmit a ping
 */

int is2_out_ping(struct worker_t *self, struct client_t *c)
{
	ProtobufCBinaryData rdata;
	rdata.data = NULL;
	rdata.len  = 0; 
	
	KeepalivePing ping = KEEPALIVE_PING__INIT;
	ping.ping_type = KEEPALIVE_PING__PING_TYPE__REQUEST;
	ping.request_id = random();
	ping.request_data = rdata;
	
	IS2Message m = IS2_MESSAGE__INIT;
	m.type = IS2_MESSAGE__TYPE__KEEPALIVE_PING;
	m.keepalive_ping = &ping;
	
	return is2_write_message(self, c, &m);
}

/*
 *	Incoming ping handler, responds with a reply when a request is received
 */

static int is2_in_ping(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	int r = 0;
	
	KeepalivePing *ping = m->keepalive_ping;
	if (!ping) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of ping failed",
			c->addr_rem, c->username);
		r = -1;
		goto done;
	}
	
	hlog(LOG_DEBUG, "%s/%s: IS2: Ping %s received: request_id %lu",
		c->addr_rem, c->username,
		(ping->ping_type == KEEPALIVE_PING__PING_TYPE__REQUEST) ? "Request" : "Reply",
		ping->request_id);
	
	if (ping->ping_type == KEEPALIVE_PING__PING_TYPE__REQUEST) {
		ping->ping_type = KEEPALIVE_PING__PING_TYPE__REPLY;
		
		r = is2_write_message(self, c, m);
	}
	
done:	
	is2_message__free_unpacked(m, NULL);
	return r;
}

/*
 *	IS2 input handler, when waiting for an upstream server to
 *	transmit a server signature
 */

int is2_input_handler_uplink_wait_signature(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	switch (m->type) {
		case IS2_MESSAGE__TYPE__SERVER_SIGNATURE:
			return is2_in_server_signature(self, c, m);
		case IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
		case IS2_MESSAGE__TYPE__LOGIN_REPLY:
			return is2_in_login_reply(self, c, m);
		default:
			hlog(LOG_WARNING, "%s/%s: IS2: connect: unknown message type %d",
				c->addr_rem, c->username, m->type);
			client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
	};
	
	is2_message__free_unpacked(m, NULL);
	return 0;
}

/*
 *	IS2 input handler, when waiting for a login command
 */
 
int is2_input_handler_login(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	switch (m->type) {
		case IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
			break;
		case IS2_MESSAGE__TYPE__LOGIN_REQUEST:
			return is2_in_login_request(self, c, m);
			break;
		default:
			hlog(LOG_WARNING, "%s/%s: IS2: login: unknown message type %d",
				c->addr_rem, c->username, m->type);
			break;
	};
	
	return 0;
}

/*
 *	IS2 input handler, connected state
 */
 
int is2_input_handler(struct worker_t *self, struct client_t *c, IS2Message *m)
{
	switch (m->type) {
		case IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
			break;
		case IS2_MESSAGE__TYPE__IS_PACKET:
			return is2_in_packet(self, c, m);
			break;
		default:
			hlog(LOG_WARNING, "%s/%s: IS2: unknown message type %d",
				c->addr_rem, c->username, m->type);
			break;
	};
	
	return 0;
}

/*
 *	Unpack a single message from the input buffer.
 */

static int is2_unpack_message(struct worker_t *self, struct client_t *c, void *buf, int len)
{
	IS2Message *m = is2_message__unpack(NULL, len, buf);
	if (!m) {
		hlog_packet(LOG_WARNING, buf, len, "%s/%s: IS2: unpacking of message failed: ",
			c->addr_rem, c->username);
		return 0;
	}
	
	/* Call the current input message handler */
	return c->is2_input_handler(self, c, m);
}

/*
 *	Scan client input buffer for valid IS2 frames, and
 *	process them.
 */

#define IS2_MINIMUM_FRAME_CONTENT_LEN 4
#define IS2_MINIMUM_FRAME_LEN (IS2_HEAD_LEN + IS2_MINIMUM_FRAME_CONTENT_LEN + IS2_TAIL_LEN)

int is2_deframe_input(struct worker_t *self, struct client_t *c, int start_at)
{
	int i;
	char *ibuf = c->ibuf;
	
	for (i = start_at; i < c->ibuf_end; ) {
		int left = c->ibuf_end - i;
		char *this = &ibuf[i];
		
		if (left < IS2_MINIMUM_FRAME_LEN) {
			//hlog_packet(LOG_DEBUG, this, left, "%s/%s: IS2: Don't have enough data in buffer yet (%d): ", c->addr_rem, c->username, left);
			break;
		}
		
		if (*this != STX) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame missing STX in beginning: ",
				c->addr_rem, c->username);
			client_close(self, c, CLIERR_IS2_FRAMING_NO_STX);
			return -1;
		}
		
		uint32_t *ip = (uint32_t *)this;
		uint32_t clen = ntohl(*ip) & 0xffffff;
		
		if (clen < IS2_MINIMUM_FRAME_CONTENT_LEN) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Too short frame content (%d): ",
				c->addr_rem, c->username, clen);
			return -1;
		}
		
		if (IS2_HEAD_LEN + clen + IS2_TAIL_LEN > left) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame length points behind buffer end (%d+%d buflen %d): ",
				c->addr_rem, c->username, clen, IS2_HEAD_LEN + IS2_TAIL_LEN, left);
			/* this might get fixed when more data comes out from the pipe */
			break;
		}
		
		if (this[IS2_HEAD_LEN + clen] != ETX) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame missing terminating ETX: ",
				c->addr_rem, c->username);
			return -1;
		}
		
		hlog_packet(LOG_DEBUG, this, left, "%s/%s: IS2: framing ok: ", c->addr_rem, c->username);
		
		is2_unpack_message(self, c, this + IS2_HEAD_LEN, clen);
		i += IS2_HEAD_LEN + clen + IS2_TAIL_LEN;
	}
	
	return i;
}

/*
 *	Write a single packet to a client.
 *
 *	OPTIMIZE: generate packet once, or reuse incoming prepacked buffer
 */

int is2_write_packet(struct worker_t *self, struct client_t *c, char *p, int len)
{
	/* trim away CR/LF */
	len = len - 2;
	
	hlog(LOG_DEBUG, "%s/%s: IS2: writing IS packet of %d bytes", c->addr_rem, c->username, len);
	
	ProtobufCBinaryData data;
	data.data = (uint8_t *)p;
	data.len  = len;
	
	int n = 1; // just one packet
	int i;
	ISPacket **subs = hmalloc(sizeof(ISPacket*) * n);
	for (i = 0; i < n; i++) {
		hlog(LOG_DEBUG, "packing packet %d", i);
		subs[i] = hmalloc(sizeof(ISPacket));
		ispacket__init(subs[i]);
		subs[i]->type = ISPACKET__TYPE__IS_PACKET;
		subs[i]->has_is_packet_data = 1;
		subs[i]->is_packet_data = data;
	}
	
	IS2Message m = IS2_MESSAGE__INIT;
	m.type = IS2_MESSAGE__TYPE__IS_PACKET;
	m.n_is_packet = n;
	m.is_packet = subs;
	
	int ret = is2_write_message(self, c, &m);
	
	for (i = 0; i < n; i++)
		hfree(subs[i]);
	hfree(subs);
	
	return ret;
}

