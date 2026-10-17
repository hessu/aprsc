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
 *	aprsis2.c: the APRS-IS2 Protocol Buffers based protocol: framing,
 *	login, keepalives, output buffering, UDP peer negotiation and
 *	frame signatures
 */

#include <string.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

#include "hmalloc.h"
#include "worker.h"
#include "aprsis2.h"
#include "aprsis2.pb-c.h"
#include "aprsis2plain.h"
#include "version.h"
#include "hlog.h"
#include "uplink.h"
#include "config.h"
#include "parse_qc.h"
#include "passcode.h"
#include "clientlist.h"
#include "login.h"
#include "incoming.h"
#include "filter.h"
#include "random.h"
#include "rwlock.h"


#define IS2_HEAD_LEN 4 /* STX + reserved 8 bits + network byte order 16 bits uint to specify body length */
#define IS2_TAIL_LEN 1 /* ETX */

#define IS2_MINIMUM_FRAME_CONTENT_LEN 4
#define IS2_MINIMUM_FRAME_LEN (IS2_HEAD_LEN + IS2_MINIMUM_FRAME_CONTENT_LEN + IS2_TAIL_LEN)
#define IS2_MAXIMUM_FRAME_LEN 4096
#define IS2_MAXIMUM_FRAME_CONTENT_LEN (IS2_MAXIMUM_FRAME_LEN - IS2_HEAD_LEN - IS2_TAIL_LEN)

#define IS2_UDP_HMAC_MARKER 0x05
#define IS2_UDP_HMAC_START 6 /* marker + key id + 32-bit timestamp, all covered by the HMAC */
#define IS2_UDP_HMAC_TRAILER_LEN (IS2_UDP_HMAC_START + 32) /* ... + HMAC-SHA256 */
#define IS2_UDP_FRAME_MAX_AGE 10 /* seconds: how far the timestamp of a frame may be from our clock */

/*
 *	Protocol Buffers wire format bits for building the IS2Message envelope
 *	around ISPacket elements by hand. The field numbers come from
 *	aprsis2.proto - is2_wire_selfcheck() verifies them against protobuf-c
 *	at startup.
 */

#define PB_WIRETYPE_VARINT 0
#define PB_WIRETYPE_LEN 2

#define IS2_MESSAGE_FIELD_TYPE 1
#define IS2_MESSAGE_FIELD_SEQUENCE 2
#define IS2_MESSAGE_FIELD_IS_PACKET 25

/* IS2 output buffer size: frame head, message type field, ISPacket elements,
 * sequence number field, ETX, and the HMAC trailer of UDP peer frames
 */
#define IS2_OBUF_SIZE (IS2_HEAD_LEN + 10 + APRSIS2_OBUF_MAX_LENGTH + 10 + IS2_TAIL_LEN + IS2_UDP_HMAC_TRAILER_LEN)

static int pb_put_varint(char *buf, uint64_t v)
{
	int i = 0;

	while (v >= 0x80) {
		buf[i++] = (char)((v & 0x7f) | 0x80);
		v >>= 7;
	}
	buf[i++] = (char)v;

	return i;
}

static int pb_varint_len(uint64_t v)
{
	int i = 1;

	while (v >= 0x80) {
		v >>= 7;
		i++;
	}

	return i;
}

static int pb_put_tag(char *buf, int field, int wiretype)
{
	return pb_put_varint(buf, ((uint64_t)field << 3) | wiretype);
}

/*
 *	IS2 client / port / protocol accounting
 */

static void is2_clientaccount_add_rx(struct client_t *c, int l4proto, int rx_frames, int lost_frames, int rejected_frames)
{
	struct portaccount_t *pa = NULL;
	
	/* worker local accounters do not need locks */
	c->localaccount.is2_rx_frames += rx_frames;
	c->localaccount.is2_lost_frames += lost_frames;
	c->localaccount.is2_rejected_frames += rejected_frames;
	
	if (l4proto == IPPROTO_UDP && c->udpclient && c->udpclient->portaccount) {
		pa = c->udpclient->portaccount;
	} else if (c->portaccount) {
		pa = c->portaccount;
	}
	
	if (pa) {
#ifdef HAVE_SYNC_FETCH_AND_ADD
		__sync_fetch_and_add(&pa->is2_rx_frames, rx_frames);
		__sync_fetch_and_add(&pa->is2_lost_frames, lost_frames);
		__sync_fetch_and_add(&pa->is2_rejected_frames, rejected_frames);
#else
		// FIXME: MUTEX !! -- this may or may not need locks..
		pa->is2_rx_frames += rx_frames;
		pa->is2_lost_frames += lost_frames;
		pa->is2_rejected_frames += rejected_frames;
#endif
	}
	
	struct portaccount_t *proto;
	
	if (l4proto == IPPROTO_TCP)
		proto = &client_connects_tcp;
	else if (l4proto == IPPROTO_UDP)
		proto = &client_connects_udp;
#ifdef USE_SCTP
	else if (l4proto == IPPROTO_SCTP)
		proto = &client_connects_sctp;
#endif
	else
		return;
	
#ifdef HAVE_SYNC_FETCH_AND_ADD
	__sync_fetch_and_add(&proto->is2_rx_frames, rx_frames);
	__sync_fetch_and_add(&proto->is2_lost_frames, lost_frames);
	__sync_fetch_and_add(&proto->is2_rejected_frames, rejected_frames);
#else
	// FIXME: MUTEX !! -- this may or may not need locks..
	proto->is2_rx_frames += rx_frames;
	proto->is2_lost_frames += lost_frames;
	proto->is2_rejected_frames += rejected_frames;
#endif
}


/*
 *	Allocate a buffer for a message, fill with head an tail
 */

static void is2_setup_buffer(char *buf, uint16_t len)
{
	uint16_t *len_p = (uint16_t *)&buf[2];

	buf[0] = STX;
	buf[1] = 0; // reserved
	*len_p = htons(len);

	buf[IS2_HEAD_LEN + len] = ETX;
}

#if 0 // unused at the moment
static void *is2_allocate_buffer(int len)
{
	/* total length of outgoing buffer */
	int nlen = len + IS2_HEAD_LEN + IS2_TAIL_LEN;
	
	char *buf = hmalloc(nlen);
	
	is2_setup_buffer(buf, len);
	
	return (void *)buf;
}
#endif

/*
 *	The IS2 output buffer of a client holds a partially built IS2 frame:
 *	room for the frame header, the IS2Message type field, and then the
 *	elements of the repeated is_packet field, serialized and appended one
 *	by one as packets are passed to the client. On flush, the sequence
 *	number (UDP peers only) and ETX are appended and the header is filled in.
 *
 *	Protocol Buffers allows the elements of a repeated field to be written
 *	separately, and the fields of a message in any order, so the result is
 *	equivalent to packing a complete IS2Message at once.
 *
 *	Since the buffer only contains serialized bytes, it does not refer to
 *	pbuf_t memory, which the dupecheck thread may recycle before the
 *	buffer is flushed.
 */

static void is2_obuf_reset(struct client_t *c)
{
	int i = IS2_HEAD_LEN;

	i += pb_put_tag(c->is2_obuf + i, IS2_MESSAGE_FIELD_TYPE, PB_WIRETYPE_VARINT);
	i += pb_put_varint(c->is2_obuf + i, APRSIS2__IS2_MESSAGE__TYPE__IS_PACKET);

	c->is2_obuf_end = i;
	c->is2_obuf_packets = 0;
	c->is2_obuf_total_len = 0;
}

void is2_allocate_obuf(struct client_t *c)
{
	// deallocated in client_free()
	if (!c->is2_obuf)
		c->is2_obuf = hmalloc(IS2_OBUF_SIZE);

	is2_obuf_reset(c);
}

/*
 *	Write a message to a client, return result from c->write
 */

static int is2_corepeer_write_message(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m);

static int is2_write_message(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	char buf[IS2_MAXIMUM_FRAME_LEN];
	
	if (c->state == CSTATE_COREPEER)
		return is2_corepeer_write_message(self, c, m);
	
	int len = aprsis2__is2_message__get_packed_size(m);
	int blen = len + IS2_HEAD_LEN + IS2_TAIL_LEN;
	
	if (blen > IS2_MAXIMUM_FRAME_LEN) {
		hlog(LOG_DEBUG, "%s/%s: IS2: serialized IS2 frame length %d too large, > %d", c->addr_rem, c->username, blen, IS2_MAXIMUM_FRAME_LEN);
		return -1;
	}
	
	//hlog(LOG_DEBUG, "%s/%s: IS2: serialized length %d", c->addr_rem, c->username, len);
	is2_setup_buffer(buf, len);
	aprsis2__is2_message__pack(m, (void *)buf + IS2_HEAD_LEN);
	
	return c->write(self, c, buf, blen);
}

/*
 *	Write an UDP message to a client, return result from c->write
 */

/*
 *	Keys for signing and verifying UDP peer frames, indexed by key id.
 *	The main thread builds the table from the configuration and
 *	replaces it as a whole on reload, while a worker thread may be
 *	using a key, so the lock is held for the duration of a signature
 *	calculation. The HMAC is computed with the one-shot API, so no
 *	OpenSSL context is shared between threads.
 */

struct is2_corepeer_key_t {
	unsigned char *key;
	int key_len;
};

static struct is2_corepeer_key_t *is2_corepeer_keys[IS2_PEERGROUP_KEY_ID_NUM];
static rwlock_t is2_corepeer_keys_lock = RWL_INITIALIZER;

#define IS2_HMAC_FAILED -1
#define IS2_HMAC_NO_KEY -2

/*
 *	Calculate the HMAC-SHA256 of a buffer with the given key.
 *	Returns 0 on success, IS2_HMAC_NO_KEY if the key is not configured.
 */

static int is2_corepeer_hmac(unsigned int key_id, const char *data, int len, unsigned char *md, unsigned int *md_len)
{
	struct is2_corepeer_key_t *k;
	int r = IS2_HMAC_FAILED;

	rwl_rdlock(&is2_corepeer_keys_lock);

	k = is2_corepeer_keys[key_id];
	if (!k) {
		r = IS2_HMAC_NO_KEY;
	} else {
#ifdef HAVE_OPENSSL_3
		size_t outl = 0;
		if (EVP_Q_mac(NULL, "HMAC", NULL, "SHA256", NULL, k->key, k->key_len,
		    (const unsigned char *)data, len, md, 32, &outl) != NULL) {
			*md_len = outl;
			r = 0;
		}
#else
		if (HMAC(EVP_sha256(), k->key, k->key_len, (const unsigned char *)data, len, md, md_len) != NULL)
			r = 0;
#endif
	}

	rwl_rdunlock(&is2_corepeer_keys_lock);

	return r;
}

static int is2_corepeer_message_sign(struct client_t *c, char *obuf, int obuf_size, int data_len)
{
	unsigned int key_id = peergroup_key_transmit;
	int hmac_start = data_len + IS2_UDP_HMAC_START;
	uint32_t ts;

	if (data_len + IS2_UDP_HMAC_TRAILER_LEN > obuf_size) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: no room for HMAC after %d bytes of frame in buffer of %d bytes", c->addr_rem, c->username, data_len, obuf_size);
		return -1;
	}

	/* The trailer starts with the marker, key identifier and the current
	 * time, which are covered by the HMAC together with the frame. The
	 * receiver drops frames whose time is too far off, so that captured
	 * frames can not be replayed later.
	 */
	ts = htonl((uint32_t)now);
	obuf[data_len] = IS2_UDP_HMAC_MARKER; // marker: has HMAC
	obuf[data_len+1] = key_id; // HMAC key identifier
	memcpy(&obuf[data_len+2], &ts, sizeof(ts));

	/* Calculate the MAC */
	unsigned char md[32];
	unsigned int md_len = 0;

	int r = is2_corepeer_hmac(key_id, obuf, hmac_start, md, &md_len);
	if (r == IS2_HMAC_NO_KEY) {
		hlog(LOG_DEBUG, "%s/%s: IS2 UDP: PeerGroupKey/PeerGroupKeyTransmit mismatch: Key %d not defined", c->addr_rem, c->username, key_id);
		return -1;
	}
	if (r != 0 || md_len != 32) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: HMAC calculation failed (key %d)", c->addr_rem, c->username, key_id);
		return -1;
	}

	memcpy(&obuf[hmac_start], md, md_len);

	return hmac_start + md_len;
}

/*
 *	Write a single message, such as a ping, to an UDP peer in a frame of
 *	its own: numbered like the packet frames, so that the peer counts
 *	frame loss right, and signed. Returns the result of udp_client_write.
 */

static int is2_corepeer_write_message(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	char buf[IS2_MAXIMUM_FRAME_LEN + IS2_UDP_HMAC_TRAILER_LEN];
	int len, blen, signed_len;
	
	/* protobuf-c does not transmit a zero, the first number is skipped */
	m->sequence = c->corepeer_is2_sequence++;
	
	len = aprsis2__is2_message__get_packed_size(m);
	blen = len + IS2_HEAD_LEN + IS2_TAIL_LEN;
	if (blen > IS2_MAXIMUM_FRAME_LEN) {
		hlog(LOG_DEBUG, "%s/%s: IS2 UDP: serialized IS2 frame length %d too large, > %d", c->addr_rem, c->username, blen, IS2_MAXIMUM_FRAME_LEN);
		return -1;
	}
	
	is2_setup_buffer(buf, len);
	aprsis2__is2_message__pack(m, (void *)buf + IS2_HEAD_LEN);
	
	signed_len = is2_corepeer_message_sign(c, buf, sizeof(buf), blen);
	if (signed_len < IS2_MINIMUM_FRAME_LEN)
		return -1;
	
	return udp_client_write(self, c, buf, signed_len);
}

/*
 *	Fall back to the plain UDP peer protocol, when the pings of an IS2
 *	peer link have gone unanswered for IS2_COREPEER_PING_FAIL seconds:
 *	the peer may have been downgraded to software without IS2, as
 *	doc/IS2PEERING.md says. Packets waiting in the IS2 output buffer
 *	go out first. The upgrade is proposed again, with the usual backoff,
 *	so that a peer which merely went quiet for a while comes back to IS2.
 */

void is2_corepeer_downgrade(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_WARNING, "%s/%s: IS2 UDP peer: no ping reply in %d seconds, falling back to the plain UDP protocol",
		c->addr_rem, c->username, (int)(tick - c->ping_reply_tick));
	
	if (c->is2_obuf_packets)
		is2_corepeer_obuf_flush(self, c);
	
	c->corepeer_is2 = 0;
	c->write_packet = &udp_client_write_packet;
	c->ping_rtt_n = 0;
	c->ping_rtt_i = 0;
	
	c->next_is2_peer_interval = COREPEER_IS2_PROPOSE_T_MIN;
	/* due already, so that the proposal goes out on the same keepalive
	 * pass which did the fallback: the check is a strict comparison */
	c->next_is2_peer_offer = tick - 1;
}

/*
 *	Start the ping tracking and the frame loss tracking of an IS2 UDP peer
 *	link afresh when it is upgraded to IS2. No round-trip times are left
 *	over from an earlier IS2 period of the link, and the fallback timer
 *	starts over.
 *
 *	After a fallback and this re-upgrade, the sequence numbers of the peer
 *	have moved on past the window, or started over with the peer, and the
 *	frames of the gap were dropped on purpose, not lost.
 */

static void is2_corepeer_track_reset(struct client_t *c)
{
	c->ping_reply_tick = tick;
	c->ping_rtt_n = 0;
	c->ping_rtt_i = 0;
	
	c->corepeer_is2_sequence_window_pos = 0;
	c->corepeer_is2_sequence_window_used = 0;
}

/*
 *	Transmit a server signature to a new client
 */

int is2_out_server_signature(struct worker_t *self, struct client_t *c)
{
	Aprsis2__IS2ServerSignature sig = APRSIS2__IS2_SERVER_SIGNATURE__INIT;
	sig.username = serverid;
	sig.app_name = verstr_progname;
	sig.app_version = version_build;
	sig.n_features = 0;
	sig.features = NULL;
	
	Aprsis2__IS2Message m = APRSIS2__IS2_MESSAGE__INIT;
	m.type = APRSIS2__IS2_MESSAGE__TYPE__SERVER_SIGNATURE;
	m.server_signature = &sig;
	
	return is2_write_message(self, c, &m);
}

/*
 *	Transmit a login reply to a new client
 */

int is2_out_login_reply(struct worker_t *self, struct client_t *c, Aprsis2__IS2LoginReply__LoginResult result, Aprsis2__IS2LoginReply__LoginResultReason reason, Aprsis2__VerificationStatus verified, const char *message)
{
	Aprsis2__IS2LoginReply lr = APRSIS2__IS2_LOGIN_REPLY__INIT;
	lr.result = result;
	lr.result_code = reason;
	lr.verified = verified;
	if (message)
		lr.result_message = (char *)message;

	Aprsis2__IS2Message m = APRSIS2__IS2_MESSAGE__INIT;
	m.type = APRSIS2__IS2_MESSAGE__TYPE__LOGIN_REPLY;
	m.login_reply = &lr;
	
	return is2_write_message(self, c, &m);
}

/*
 *	Receive a login reply
 */

static int is2_in_login_reply(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	Aprsis2__IS2LoginReply *lr = m->login_reply;
	if (!lr) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of login reply failed",
			c->addr_rem, c->username);
		return -1;
	}
	
	if (lr->result != APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT__OK) {
		hlog(LOG_INFO, "%s/%s: IS2: Login failed: '%s' (%d)",
			c->addr_rem, c->username,
			(lr->result_message) ? lr->result_message : "no reason", lr->result_code);
		return -1;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Login reply received",
		c->addr_rem, c->username);

	is2_allocate_obuf(c);

	/* ok, login succeeded, switch handler */
	c->is2_input_handler = &is2_input_handler;
	
	/* mark as connected and classify */
	worker_mark_client_connected(self, c);
	
	return 0;
}


/*
 *	Received server signature from an upstream server
 *	- if ok, continue by sending a login command
 */

static int is2_in_server_signature(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	Aprsis2__IS2ServerSignature *sig = m->server_signature;
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

	if (strcasecmp(sig->username, serverid) == 0) {
		hlog(LOG_ERR, "%s: Uplink's server name is same as ours: '%s'", c->addr_rem, sig->username);
		client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
		return -1;
	}
	
	/* TODO: validate server callsign with the q valid path algorithm */
	
	/* store the remote server's callsign as the "client username" */
	strncpy(c->username, sig->username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	
	/* uplink servers are always "validated" */
	c->validated = VALIDATED_WEAK;
	
#ifdef USE_SSL
	if (!uplink_server_validate_cert(self, c) || !uplink_server_validate_cert_cn(self, c))
		return -1;
#endif
	
	/* Ok, we're happy with the uplink's server signature, let us login! */
	Aprsis2__IS2LoginRequest lr = APRSIS2__IS2_LOGIN_REQUEST__INIT;
	lr.username = serverid;
	lr.password = passcode;
	lr.app_name = verstr_progname;
	lr.app_version = version_build;
	lr.n_features_req = 0;
	lr.features_req = NULL;
	
	Aprsis2__IS2Message mr = APRSIS2__IS2_MESSAGE__INIT;
	mr.type = APRSIS2__IS2_MESSAGE__TYPE__LOGIN_REQUEST;
	mr.login_request = &lr;
	
	int rc = is2_write_message(self, c, &mr);
	if (rc < -2)
		return rc; /* the client was destroyed by the write */

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

static int is2_in_login_request(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	int rc = 0;
	int i;
	Aprsis2__IS2LoginReply__LoginResultReason reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__NONE;
	const char *reason_s = "Login failed";
	
	Aprsis2__IS2LoginRequest *lr = m->login_request;
	if (!lr) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of login request failed",
			c->addr_rem, c->username);
		reason_s = "Invalid login request";
		goto failed_login;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Login request received",
		c->addr_rem, c->username);
	
	/* limit username length */
	if (strlen(lr->username) > CALLSIGNLEN_MAX) {
		hlog(LOG_WARNING, "%s: IS2: Invalid login request, too long username: '%.20s'", c->addr_rem, lr->username);
		reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__INVALID_USERNAME;
		reason_s = "Invalid username format";
		goto failed_login;
	}
	
	/* ok, it's somewhat valid, write it down */
	strncpy(c->username, lr->username, sizeof(c->username));
	c->username[sizeof(c->username)-1] = 0;
	c->username_len = strlen(c->username);
	
	/* grab app information */
	login_set_app_name(c, lr->app_name, lr->app_version);
	
	/* check the username against a static list of disallowed usernames */
	for (i = 0; (disallow_login_usernames[i]); i++) {
		if (strcasecmp(c->username, disallow_login_usernames[i]) == 0) {
			hlog(LOG_WARNING, "%s: IS2: Login by user '%s' not allowed", c->addr_rem, c->username);
			reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__USERNAME_NOT_ALLOWED;
			reason_s = "Login by user not allowed";
			goto failed_login;
		}
	}

	/* check the username against a dynamic list of disallowed usernames */
	if (disallow_login_glob && check_call_glob_match(disallow_login_glob, c->username, c->username_len)) {
		hlog(LOG_WARNING, "%s: IS2: Login by user '%s' not allowed due to config", c->addr_rem, c->username);
		reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__USERNAME_NOT_ALLOWED;
		reason_s = "Login by user not allowed";
		goto failed_login;
	}
	
	/* make sure the callsign is OK on the APRS-IS */
	if (check_invalid_q_callsign(c->username, c->username_len)) {
		hlog(LOG_WARNING, "%s: IS2: Invalid login request, invalid username: '%s'", c->addr_rem, c->username);
		reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__INVALID_USERNAME;
		reason_s = "Invalid username format, not allowed";
		goto failed_login;
	}
	
	/* make sure the client's callsign is not my Server ID */
	if (strcasecmp(c->username, serverid) == 0) {
		hlog(LOG_WARNING, "%s: IS2: Invalid login request, username equals our serverid: '%s'", c->addr_rem, c->username);
		reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__USERNAME_NOT_ALLOWED;
		reason_s = "Login by user not allowed (our serverid)";
		goto failed_login;
	}
	
	/* if SSL client cert verification is enabled, check it */
	int ssl_validated = 0;
#ifdef USE_SSL
	if (c->ssl_con && c->ssl_con->validate) {
		ssl_validated = is2_login_client_validate_cert(self, c);
		reason = APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__CERT_VERIFICATION_FAIL;
		reason_s = "Client certificate validation failed";
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
	
	Aprsis2__VerificationStatus vs;
	switch (c->validated) {
	case VALIDATED_WEAK:
		vs = APRSIS2__VERIFICATION_STATUS__WEAK;
		break;
	case VALIDATED_STRONG:
		vs = APRSIS2__VERIFICATION_STATUS__STRONG;
		break;
	default:
		vs = APRSIS2__VERIFICATION_STATUS__NONE;
	}
	
	/* tell the client he's good */
	rc = is2_out_login_reply(self, c, APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT__OK, APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT_REASON__NONE, vs, NULL);
	if (rc < -2)
		return rc; /* the client was destroyed by the write */

	is2_allocate_obuf(c);
	
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

	return 0;

failed_login:
	
	/* if we already lost the client, just return */
	if (rc < -2)
		return rc;
	
	/* tell the client why, instead of leaving it waiting */
	rc = is2_out_login_reply(self, c, APRSIS2__IS2_LOGIN_REPLY__LOGIN_RESULT__FAIL, reason, APRSIS2__VERIFICATION_STATUS__NONE, reason_s);
	if (rc < -2)
		return rc; /* the client was destroyed by the write */

	c->failed_cmds++;
	if (c->failed_cmds >= 3) {
		client_close(self, c, CLIERR_LOGIN_RETRIES);
		return -3;
	}
	
	/* the request has been handled, the client may try again */
	return 0;
}

/*
 *	Incoming packet handler
 */

static int is2_in_packet(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	int i;
	Aprsis2__ISPacket *p;
	
	//hlog(LOG_DEBUG, "%s/%s: IS2: %d packets received in message", c->addr_rem, c->username, m->n_is_packet);
	
	for (i = 0; i < m->n_is_packet; i++) {
		p = m->is_packet[i];
		
		//hlog(LOG_DEBUG, "%s/%s: IS2: packet type %d len %d", c->addr_rem, c->username, p->type, p->is_packet_data.len);
		
		if (p->type == APRSIS2__ISPACKET__TYPE__IS_PACKET && p->is_packet_data.len > 0) {
			is2_incoming_handler(self, c, c->ai_protocol, p); // always returns 0, ignores erroneus packets
		}
	}
	
	return 0;
}


/*
 *	The average and the maximum round-trip time of the last IS2 pings
 *	of a client, in seconds, over the IS2_PING_WINDOW samples kept,
 *	and the period the samples cover: the seconds from the oldest
 *	sample to now. Returns the number of samples the values are from,
 *	0 when there has been no ping reply yet.
 */

int is2_ping_rtt_stats(struct client_t *c, double *avg, double *max, time_t *period)
{
	double sum = 0.0;
	double m = 0.0;
	int i;
	
	for (i = 0; i < c->ping_rtt_n; i++) {
		sum += c->ping_rtt[i];
		if (c->ping_rtt[i] > m)
			m = c->ping_rtt[i];
	}
	
	if (c->ping_rtt_n > 0) {
		*avg = sum / c->ping_rtt_n;
		/* the oldest sample is the one the next one will overwrite,
		 * once the ring is full; before that, the first one */
		i = (c->ping_rtt_n == IS2_PING_WINDOW) ? c->ping_rtt_i : 0;
		*period = tick - c->ping_rtt_tick[i];
	} else {
		*avg = 0.0;
		*period = 0;
	}
	*max = m;
	
	return c->ping_rtt_n;
}

/*
 *	Transmit a ping
 */

int is2_out_ping(struct worker_t *self, struct client_t *c)
{
	ProtobufCBinaryData rdata;

#ifdef USE_CLOCK_GETTIME
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	rdata.data = (void *)&ts;
	rdata.len  = sizeof(ts);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	rdata.data = (void *)&tv;
	rdata.len  = sizeof(tv);
#endif
	
	c->ping_timeout = tick + IS2_PING_TIMEOUT;
	
	Aprsis2__IS2KeepalivePing ping = APRSIS2__IS2_KEEPALIVE_PING__INIT;
	ping.ping_type = APRSIS2__IS2_KEEPALIVE_PING__PING_TYPE__REQUEST;
	ping.request_id = c->ping_request_id = random();
	ping.request_data = rdata;
	
	Aprsis2__IS2Message m = APRSIS2__IS2_MESSAGE__INIT;
	m.type = APRSIS2__IS2_MESSAGE__TYPE__KEEPALIVE_PING;
	m.keepalive_ping = &ping;
	
	return is2_write_message(self, c, &m);
}

/*
 *	Incoming ping handler, responds with a reply when a request is received
 */

static int is2_in_ping(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	int r = 0;
	
	Aprsis2__IS2KeepalivePing *ping = m->keepalive_ping;
	if (!ping) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of ping failed",
			c->addr_rem, c->username);
		r = -1;
		goto done;
	}
	
	hlog(LOG_DEBUG, "%s/%s: IS2: Ping %s received: request_id %lu",
		c->addr_rem, c->username,
		(ping->ping_type == APRSIS2__IS2_KEEPALIVE_PING__PING_TYPE__REQUEST) ? "request" : "reply",
		(unsigned long)ping->request_id);
	
	if (ping->ping_type == APRSIS2__IS2_KEEPALIVE_PING__PING_TYPE__REQUEST) {
		ping->ping_type = APRSIS2__IS2_KEEPALIVE_PING__PING_TYPE__REPLY;
		
		r = is2_write_message(self, c, m);
		if (r < -2)
			return r; /* the client was destroyed by the write */
	
		/* a buffered or failed write does not make the ping invalid */
		r = 0;
	} else if (ping->ping_type == APRSIS2__IS2_KEEPALIVE_PING__PING_TYPE__REPLY && ping->request_id == c->ping_request_id) {
		double diff;
#ifdef USE_CLOCK_GETTIME
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);

		if (ping->request_data.len != sizeof(ts)) {
			hlog(LOG_WARNING, "%s/%s: IS2: ping reply data of wrong size (got %zu expected %zu)",
				c->addr_rem, c->username, ping->request_data.len, sizeof(ts));
			return -1;
		}
		
		struct timespec *tx_ts = (void *)ping->request_data.data;
		diff = (ts.tv_sec - tx_ts->tv_sec) + ((ts.tv_nsec - tx_ts->tv_nsec) / 1000000000.0);
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);

		if (ping->request_data.len != sizeof(tv)) {
			hlog(LOG_WARNING, "%s/%s: IS2: ping reply data of wrong size (got %zu expected %zu)",
				c->addr_rem, c->username, ping->request_data.len, sizeof(tv));
			return -1;
		}
		
		struct timeval *tx_tv = (void *)ping->request_data.data;
		diff = (tv.tv_sec - tx_tv->tv_sec) + ((tv.tv_usec - tx_tv->tv_usec) / 1000000.0);
#endif
		c->ping_reply_tick = tick;
		
		/* keep the sample in the ring of the last ones, for the average and max */
		c->ping_rtt[c->ping_rtt_i] = diff;
		c->ping_rtt_tick[c->ping_rtt_i] = tick;
		c->ping_rtt_i = (c->ping_rtt_i + 1) % IS2_PING_WINDOW;
		if (c->ping_rtt_n < IS2_PING_WINDOW)
			c->ping_rtt_n++;
		
		double avg, max;
		time_t period;
		int n = is2_ping_rtt_stats(c, &avg, &max, &period);
		hlog(LOG_DEBUG, "%s/%s: IS2: Ping reply received, rtt %.1f ms, avg %.1f ms max %.1f ms of the last %d in %ld s",
			c->addr_rem, c->username, diff * 1000.0, avg * 1000.0, max * 1000.0, n, (long)period);
		
	}
	
done:	
	return r;
}

/*
 *	Incoming parameter set handler
 */

static int is2_in_parameter(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	int r = 0;
	
	Aprsis2__IS2Parameter *par = m->parameter;
	if (!par) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of parameter message failed: no parameter payload",
			c->addr_rem, c->username);
		r = -1;
		goto done;
	}
	
	if (par->type != APRSIS2__IS2_PARAMETER__TYPE__PARAMETER_SET) {
		hlog(LOG_WARNING, "%s/%s: IS2: unpacking of parameter message failed: wrong type %d (not PARAMETER_SET)",
			c->addr_rem, c->username, par->type);
		r = -1;
		goto done;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Parameter set received: request_id %d%s",
		c->addr_rem, c->username, par->request_id,
		(par->filter_string) ? " filter_string" : "");
	
	/* prepare reply message */
	Aprsis2__IS2Parameter prep = APRSIS2__IS2_PARAMETER__INIT;
	prep.type = APRSIS2__IS2_PARAMETER__TYPE__PARAMETER_FAILED;
	prep.request_id = par->request_id;
	Aprsis2__IS2Message rm = APRSIS2__IS2_MESSAGE__INIT;
	rm.type = APRSIS2__IS2_MESSAGE__TYPE__PARAMETER;
	rm.parameter = &prep;
	
	if (par->filter_string && !(c->flags & CLFLAGS_USERFILTEROK)) {
		/* like the plaintext filter command, user filters are only
		 * accepted on ports which allow them
		 */
		hlog(LOG_INFO, "%s/%s: IS2: PARAMETER_SET: user filters are not allowed on this port",
			c->addr_rem, c->username);
	} else if (par->filter_string) {
		filter_set(c, par->filter_string, strlen(par->filter_string));
		
		prep.type = APRSIS2__IS2_PARAMETER__TYPE__PARAMETER_APPLIED;
		prep.filter_string = c->filter_s;
	} else {
		hlog(LOG_WARNING, "%s/%s: IS2: PARAMETER_SET: No parameters found for setting",
			c->addr_rem, c->username);
	}
	
	r = is2_write_message(self, c, &rm);
	if (r < -2)
		return r; /* the client was destroyed by the write */

	/* a buffered or failed write does not make the request invalid */
	r = 0;
	
done:	
	return r;
}

/*
 *	IS2 input handler, when waiting for an upstream server to
 *	transmit a server signature
 */

int is2_input_handler_uplink_wait_signature(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	switch (m->type) {
		case APRSIS2__IS2_MESSAGE__TYPE__SERVER_SIGNATURE:
			return is2_in_server_signature(self, c, m);
		case APRSIS2__IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
		case APRSIS2__IS2_MESSAGE__TYPE__LOGIN_REPLY:
			return is2_in_login_reply(self, c, m);
		default:
			hlog(LOG_WARNING, "%s/%s: IS2: connect: unknown message type %d",
				c->addr_rem, c->username, m->type);
			client_close(self, c, CLIERR_UPLINK_LOGIN_PROTO_ERR);
			return -1;
	};
	
	return 0;
}

/*
 *	IS2 input handler, when waiting for a login command
 */

int is2_input_handler_login(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	switch (m->type) {
		case APRSIS2__IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
			break;
		case APRSIS2__IS2_MESSAGE__TYPE__LOGIN_REQUEST:
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
 
int is2_input_handler(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	switch (m->type) {
		case APRSIS2__IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
			break;
		case APRSIS2__IS2_MESSAGE__TYPE__PARAMETER:
			return is2_in_parameter(self, c, m);
			break;
		case APRSIS2__IS2_MESSAGE__TYPE__IS_PACKET:
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
 *	IS2 corepeer incoming packets: Monitor packet loss
 */

static inline void is2_input_corepeer_sequence_check(struct worker_t *self, struct client_t *c, int latest_sequence)
{
	int expected_sequence = latest_sequence - APRSIS2_COREPEER_SEQ_WINDOW;
	if (expected_sequence < 0) {
		// No handling of sequence number wrap yet
		return;
	}

	//hlog(LOG_DEBUG, "IS2 corepeer seq got %d looking for %d", latest_sequence, expected_sequence);
	for (int i = 0; i < APRSIS2_COREPEER_SEQ_WINDOW; i++) {
		int check_window_position = c->corepeer_is2_sequence_window_pos + i;
		if (check_window_position >= APRSIS2_COREPEER_SEQ_WINDOW)
			check_window_position -= APRSIS2_COREPEER_SEQ_WINDOW;
		//hlog(LOG_DEBUG, "IS2 corepeer seq checking win %d", check_window_position);
		if (c->corepeer_is2_sequence_window[check_window_position] == expected_sequence) {
			//hlog(LOG_DEBUG, "IS2 corepeer seq %d found in win %d - OK", expected_sequence, check_window_position);
			return;
		}
	}

	//hlog(LOG_DEBUG, "IS2 corepeer packet loss - missed %d", expected_sequence);
	is2_clientaccount_add_rx(c, c->ai_protocol, 0, 1, 0);
}

static inline void is2_input_corepeer_sequence_monitor(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	int latest_sequence = m->sequence;

	// If we have the full window of sequence numbers collected, check for loss
	if (c->corepeer_is2_sequence_window_used == APRSIS2_COREPEER_SEQ_WINDOW) {
		is2_input_corepeer_sequence_check(self, c, latest_sequence);
	}

	c->corepeer_is2_sequence_window[c->corepeer_is2_sequence_window_pos] = latest_sequence;
	//hlog(LOG_DEBUG, "IS2 corepeer seq win %d received %d", c->corepeer_is2_sequence_window_pos, latest_sequence);

	c->corepeer_is2_sequence_window_pos++;
	if (c->corepeer_is2_sequence_window_pos == APRSIS2_COREPEER_SEQ_WINDOW) {
		c->corepeer_is2_sequence_window_pos = 0;
		// make a note that we have a full window recorded, and start tracking loss
		c->corepeer_is2_sequence_window_used = APRSIS2_COREPEER_SEQ_WINDOW;
	}
}


/*
 *	IS2 input handler, corepeer state
 */
 
int is2_input_handler_corepeer(struct worker_t *self, struct client_t *c, Aprsis2__IS2Message *m)
{
	is2_input_corepeer_sequence_monitor(self, c, m);

	switch (m->type) {
		case APRSIS2__IS2_MESSAGE__TYPE__KEEPALIVE_PING:
			return is2_in_ping(self, c, m);
			break;
		case APRSIS2__IS2_MESSAGE__TYPE__IS_PACKET:
			return is2_in_packet(self, c, m);
			break;
		default:
			hlog(LOG_WARNING, "%s/%s: IS2 UDP: unknown message type %d",
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
	Aprsis2__IS2Message *m = aprsis2__is2_message__unpack(NULL, len, buf);
	if (!m) {
		hlog_packet(LOG_WARNING, buf, len, "%s/%s: IS2: unpacking of message failed: ",
			c->addr_rem, c->username);
		return 0;
	}
	
	is2_clientaccount_add_rx(c, c->ai_protocol, 1, 0, 0);

	/* Call the current input message handler */
	int r = c->is2_input_handler(self, c, m);
	
	aprsis2__is2_message__free_unpacked(m, NULL);
	
	return r;
}

/*
 *	Scan client input buffer for valid IS2 frames, and
 *	process them.
 */

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

		if (this[1] != 0) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame with non-zero reserved byte: ",
				c->addr_rem, c->username);
			client_close(self, c, CLIERR_IS2_FRAMING_INCOMPATIBLE);
			return -1;
		}

		uint16_t *ip = (uint16_t *)&this[2];
		uint16_t clen = ntohs(*ip);
		
		if (clen < IS2_MINIMUM_FRAME_CONTENT_LEN || clen > IS2_MAXIMUM_FRAME_CONTENT_LEN) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Invalid frame content length (%d): ",
				c->addr_rem, c->username, clen);
			client_close(self, c, CLIERR_IS2_FRAMING_INVALID);
			return -1;
		}
		
		if (IS2_HEAD_LEN + clen + IS2_TAIL_LEN > left) {
			//hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame length points behind buffer end (%d+%d buflen %d): ", c->addr_rem, c->username, clen, IS2_HEAD_LEN + IS2_TAIL_LEN, left);
			/* this might get fixed when more data comes out from the pipe, 
			 * pretty normal at high volume
			 */
			break;
		}
		
		if (this[IS2_HEAD_LEN + clen] != ETX) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame missing terminating ETX: ",
				c->addr_rem, c->username);
			client_close(self, c, CLIERR_IS2_FRAMING_INVALID);
			return -1;
		}
		
		//hlog_packet(LOG_DEBUG, this, left, "%s/%s: IS2: framing ok: ", c->addr_rem, c->username);
		
		/* The message handler may have disconnected and discarded the client:
		 * it returns -1 after closing it, or a value < -2 if a write destroyed
		 * it. Stop on any error, like the plaintext deframer does.
		 */
		int r = is2_unpack_message(self, c, this + IS2_HEAD_LEN, clen);
		if (r < 0)
			return -1;

		i += IS2_HEAD_LEN + clen + IS2_TAIL_LEN;
	}
	
	return i;
}

/*
 *	In-band upgrade of a plaintext client connection to IS2 (see
 *	doc/IS2UPGRADE.md). The client sends "#is2 hello v=1[,2...]" before
 *	its login line, and the server answers "#is2 hello v=1" and switches
 *	to IS2. The server does not announce the upgrade after its banner:
 *	the uplink code of existing aprsc releases expects the logresp right
 *	after the banner, and closes the connection on any other line.
 */

/* the range of upgrade protocol versions supported */
#define IS2_UPGRADE_VERSION_MIN 1
#define IS2_UPGRADE_VERSION_MAX 1

/*
 *	Is the upgrade offered to this client? Not on servers which pretend to
 *	be some other software, or have picky clients which are all handled
 *	in quirks mode. Only before login.
 */
int is2_upgrade_offered(struct client_t *c)
{
	return (c->flags & CLFLAGS_IS2_UPGRADE) && !(c->flags & CLFLAGS_IS2)
		&& c->state == CSTATE_LOGIN && !quirks_mode && !fake_version;
}

/* the fields of a "#is2 hello" line, not NUL-terminated */
struct is2_upgrade_hello_t {
	const char *v;
	int v_len;
};

static void is2_upgrade_hello_key(void *ctx, const char *key, int key_len, const char *val, int val_len)
{
	struct is2_upgrade_hello_t *hello = ctx;

	if (key_len == 1 && memcmp(key, "v", 1) == 0) {
		hello->v = val;
		hello->v_len = val_len;
	}
	/* unknown keys are ignored, so that later versions can add them */
}

/*
 *	After the upgrade, the client may still send plaintext lines which it
 *	sent before receiving our answer: the login line, and maybe comments.
 *	Discard them until an IS2 frame starts with STX at the beginning of a
 *	line. Lines are only skipped when complete, so that the STX is found
 *	at the start of the next one even if the line terminator arrives later.
 */
static int is2_upgrade_deframe_input(struct worker_t *self, struct client_t *c, int start_at)
{
	char *ibuf = c->ibuf;
	int i = start_at;

	while (i < c->ibuf_end) {
		if (ibuf[i] == STX) {
			c->handler_consume_input = &is2_deframe_input;
			return is2_deframe_input(self, c, i);
		}

		int eol = i;
		while (eol < c->ibuf_end && ibuf[eol] != '\r' && ibuf[eol] != '\n')
			eol++;

		if (eol >= c->ibuf_end)
			break; /* not complete yet */

		/* the line may contain the passcode, so it is not logged */
		if (eol > i)
			hlog(LOG_DEBUG, "%s: IS2 upgrade: discarding a plaintext line of %d bytes", c->addr_rem, eol - i);

		i = eol;
		while (i < c->ibuf_end && (ibuf[i] == '\r' || ibuf[i] == '\n'))
			i++;
	}

	return i;
}

/*
 *	A "#is2 hello" line from a plaintext client before login. Returns 0 if
 *	the line was ignored, and the client stays on plaintext, 1 if the
 *	connection was upgraded, or < 0 if the client was closed.
 */
int is2_upgrade_hello_in(struct worker_t *self, struct client_t *c, const char *s, int len)
{
	struct is2_upgrade_hello_t hello;

	if (!is2_upgrade_offered(c)) {
		hlog(LOG_DEBUG, "%s: IS2 upgrade: hello ignored, upgrade not offered", c->addr_rem);
		return 0;
	}

	memset(&hello, 0, sizeof(hello));
	is2_kv_parse(s + IS2_UPGRADE_HELLO_PREFIX_LEN, len - IS2_UPGRADE_HELLO_PREFIX_LEN, is2_upgrade_hello_key, &hello);

	int v = (hello.v) ? is2_kv_version_pick(hello.v, hello.v_len, IS2_UPGRADE_VERSION_MIN, IS2_UPGRADE_VERSION_MAX) : 0;
	if (!v) {
		/* no answer: the client logs in on plaintext, as it would on
		 * a server which does not know of IS2 */
		hlog(LOG_INFO, "%s: IS2 upgrade: client offered versions %.*s, none of which are supported, staying on plaintext",
			c->addr_rem, (hello.v) ? hello.v_len : 1, (hello.v) ? hello.v : "-");
		return 0;
	}

	hlog(LOG_DEBUG, "%s: IS2 upgrade: switching to IS2, v=%d", c->addr_rem, v);

	/* the last plaintext line; a result below -2 means the write destroyed
	 * the client, while -1 only means the socket would block, with the
	 * line left in the output buffer */
	int r = client_printf(self, c, "%s v=%d\r\n", IS2_UPGRADE_HELLO_PREFIX, v);
	if (r < -2)
		return r;

	c->flags |= CLFLAGS_IS2;
	is2_allocate_obuf(c);
	c->write_packet = &is2_write_packet;
	c->is2_input_handler = &is2_input_handler_login;
	c->handler_consume_input = &is2_upgrade_deframe_input;

	/* then IS2, starting with the server signature, as on an IS2 port */
	r = is2_out_server_signature(self, c);
	if (r < -2)
		return r;

	return 1;
}

static int is2_corepeer_signature_check(struct client_t *c, const char *ibuf, int len, int signature_start)
{
	int hmac_start = signature_start + IS2_UDP_HMAC_START;
	int min_signed_len = signature_start + IS2_UDP_HMAC_TRAILER_LEN;
	if (min_signed_len > len) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Frame too short to have HMAC: framed %d min %d total len %d",
			c->addr_rem, c->username, min_signed_len, min_signed_len, len);
		return -1;
	}
	
	if (ibuf[signature_start] != IS2_UDP_HMAC_MARKER) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Frame with invalid hmac marker %d",
			c->addr_rem, c->username, ibuf[signature_start]);
		return -1;
	}

	unsigned int key_id = ibuf[signature_start+1];
	if (key_id >= IS2_PEERGROUP_KEY_ID_NUM) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Frame with invalid key_id %d",
			c->addr_rem, c->username, key_id);
		return -1;
	}

	/* Calculate the MAC over the frame, marker, key id and timestamp */
	unsigned char md[32];
	unsigned int md_len = 0;

	int r = is2_corepeer_hmac(key_id, ibuf, hmac_start, md, &md_len);
	if (r == IS2_HMAC_NO_KEY) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Incoming packet with undefined key ID %d",
			c->addr_rem, c->username, key_id);
		return -1;
	}
	if (r != 0) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: HMAC calculation failed (key %d)",
			c->addr_rem, c->username, key_id);
		return -1;
	}

	//hlog(LOG_DEBUG, "IS2 rx hmac signed, key %d, sig start %d, md_len %d, received md %d bytes", key_id, signature_start, md_len, len - hmac_start);
	if (md_len != 32 || len - hmac_start != 32) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Wrong hmac length, out %d received %d",
			c->addr_rem, c->username, md_len, len - hmac_start);
		return -1;
	}
	
	if (CRYPTO_memcmp(md, &ibuf[hmac_start], md_len) != 0) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Incoming packet with wrong signature (key_id %d)", c->addr_rem, c->username, key_id);
		//hlog_packet(LOG_WARNING, (char *)md, 32, "%s/%s: calculated hmac len %d: ", c->addr_rem, c->username, md_len);
		//hlog_packet(LOG_WARNING, &ibuf[hmac_start], 32, "%s/%s: received hmac: ", c->addr_rem, c->username);
		return -1;
	}

	/* The frame is authentic. Reject it if its timestamp is too far from
	 * our clock: a captured frame can not be replayed after this window,
	 * and a replay within it is caught by the dupecheck, which remembers
	 * packets for longer than this.
	 */
	uint32_t ts;
	memcpy(&ts, &ibuf[signature_start+2], sizeof(ts));
	int64_t age = (int64_t)(uint32_t)now - (int64_t)ntohl(ts);
	if (age > IS2_UDP_FRAME_MAX_AGE || age < -IS2_UDP_FRAME_MAX_AGE) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: Frame timestamp is off by %lld seconds, dropping (clocks not in sync, or a replayed frame)",
			c->addr_rem, c->username, (long long)age);
		return -1;
	}
	
	//hlog(LOG_INFO, "%s/%s: IS2 UDP: rx hmac ok", c->addr_rem, c->username);
	return 0;
}

/*
 *	Deframe a datagram received on an IS2 UDP peer link. A datagram which
 *	is not an acceptable frame is counted as a rejected frame, so that
 *	the status shows what is going on, even when the log is quiet.
 */

int is2_corepeer_deframe_input(struct worker_t *self, struct client_t *c, char *ibuf, int len)
{
	if (len < IS2_MINIMUM_FRAME_LEN) {
		hlog_packet(LOG_DEBUG, ibuf, len, "%s/%s: IS2 UDP: Too short UDP frame (%d): ",
			c->addr_rem, c->username, len);
		is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
		return -1;
	}
	
	if (*ibuf != STX) {
		/* a plain datagram: the peer may have fallen back to the plain
		 * protocol, or restarted with software without IS2. Until the
		 * pings fail and this side falls back too, a busy peer sends
		 * many of these, so they are counted as rejected frames and
		 * logged quietly. */
		hlog_packet(LOG_DEBUG, ibuf, len, "%s/%s: IS2 UDP: Frame missing STX in beginning, plain datagram from an IS2 peer: ",
			c->addr_rem, c->username);
		is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
		return -1;
	}
	
	uint32_t *ip = (uint32_t *)ibuf;
	uint32_t clen = ntohl(*ip) & 0xffffff;
	
	if (clen < IS2_MINIMUM_FRAME_CONTENT_LEN) {
		hlog_packet(LOG_WARNING, ibuf, len, "%s/%s: IS2 UDP: Too short frame content (%d): ",
			c->addr_rem, c->username, clen);
		is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
		return -1;
	}
	
	if (IS2_HEAD_LEN + clen + IS2_TAIL_LEN > len) {
		hlog_packet(LOG_WARNING, ibuf, len, "%s/%s: IS2 UDP: Frame length points behind buffer end (%d+%d buflen %d): ", c->addr_rem, c->username, clen, IS2_HEAD_LEN + IS2_TAIL_LEN, len);
		is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
		return -1;
	}
	
	if (ibuf[IS2_HEAD_LEN + clen] != ETX) {
		hlog_packet(LOG_WARNING, ibuf, len, "%s/%s: IS2: Frame missing terminating ETX: ",
			c->addr_rem, c->username);
		is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
		return -1;
	}

	int r = is2_corepeer_signature_check(c, ibuf, len, IS2_HEAD_LEN + clen + IS2_TAIL_LEN);
	if (r < 0) {
		is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
		return r;
	}
	
	return is2_unpack_message(self, c, ibuf + IS2_HEAD_LEN, clen);
}

/*
 *	Count an IS2 frame which arrived on a UDP peer link in the plain mode,
 *	and was dropped, as a rejected frame.
 */

void is2_corepeer_plain_frame_rejected(struct client_t *c)
{
	is2_clientaccount_add_rx(c, c->ai_protocol, 0, 0, 1);
}

#ifdef USE_IS2
/*
 *	The Received entries of packets (doc/IS2RECEIVED.md): the servers which
 *	have received a packet, and who they got it from. A server appends its
 *	own entry to the packets it receives, keeping the newest
 *	IS2_RECEIVED_MAX entries, and the first server to receive a packet
 *	stamps it with the time.
 */

#define IS2_RECEIVED_MAX_AGE 30    /* seconds: a packet with an older received_time may be dropped */
#define IS2_RECEIVED_MAX_FUTURE 30 /* seconds: ... or one this far in the future */
#define IS2_RECEIVED_AGE_WARN_INTERVAL 60 /* seconds between the warnings about them, per link */

/* counters for the status page, added to by all the workers */
static long long is2_received_truncated; /* entries dropped from packets which had too many */
static long long is2_received_invalid;   /* malformed entries dropped */
static long long is2_received_loops;     /* packets which already had an entry of this server */
static long long is2_received_too_old;   /* packets with a received_time too far in the past, dropped or not */
static long long is2_received_too_new;   /* ... in the future */

static void is2_received_count(long long *counter, long long n)
{
#ifdef HAVE_SYNC_FETCH_AND_ADD
	__sync_fetch_and_add(counter, n);
#else
	// FIXME: MUTEX !! -- this may or may not need locks..
	*counter += n;
#endif
}

void is2_received_status(cJSON *totals)
{
	cJSON_AddNumberToObject(totals, "is2_received_truncated", is2_received_truncated);
	cJSON_AddNumberToObject(totals, "is2_received_invalid", is2_received_invalid);
	cJSON_AddNumberToObject(totals, "is2_received_loops", is2_received_loops);
	cJSON_AddNumberToObject(totals, "is2_received_too_old", is2_received_too_old);
	cJSON_AddNumberToObject(totals, "is2_received_too_new", is2_received_too_new);
}

/*
 *	Milliseconds since the epoch, for the received_time stamp. The time
 *	thread's 'now' is not precise enough for latency measurements.
 */

static uint64_t is2_time_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
 *	Is this a valid server_id or username of a Received entry: 1 to
 *	IS2_RECEIVED_ID_LEN printable ASCII characters?
 */

static int is2_received_id_valid(const char *s)
{
	int i;

	if (!s)
		return 0;

	for (i = 0; s[i]; i++) {
		if (i >= IS2_RECEIVED_ID_LEN)
			return 0;
		if ((unsigned char)s[i] < 0x21 || (unsigned char)s[i] > 0x7e)
			return 0;
	}

	return (i > 0);
}

/*
 *	Make room for one more entry in a packet which has IS2_RECEIVED_MAX
 *	of them: drop the oldest.
 */

static void is2_received_make_room(struct pbuf_t *pb)
{
	if (pb->received_n < IS2_RECEIVED_MAX)
		return;

	memmove(&pb->received[0], &pb->received[1], sizeof(pb->received[0]) * (IS2_RECEIVED_MAX-1));
	pb->received_n = IS2_RECEIVED_MAX-1;
	is2_received_count(&is2_received_truncated, 1);
}

/*
 *	Copy the valid Received entries of an incoming IS2 packet to the pbuf,
 *	keeping the newest IS2_RECEIVED_MAX of them, and note if the packet has
 *	been received by this server before: it has looped.
 */

static void is2_received_copy_in(struct pbuf_t *pb, struct client_t *c, Aprsis2__ISPacket *in)
{
	int i;
	int invalid = 0;
	int loop = 0;
	Aprsis2__IS2Received *r;
	struct is2_received_t *d;

	for (i = 0; i < in->n_received; i++) {
		r = in->received[i];
		if (!r
		    || !is2_received_id_valid(r->server_id)
		    || !is2_received_id_valid(r->username)
		    || r->verified < APRSIS2__VERIFICATION_STATUS__NONE
		    || r->verified > APRSIS2__VERIFICATION_STATUS__STRONG
		    || (r->client_addr.len != 0 && r->client_addr.len != 4 && r->client_addr.len != 16)) {
			invalid++;
			continue;
		}

		if (strcasecmp(r->server_id, serverid) == 0)
			loop++;

		is2_received_make_room(pb);
		d = &pb->received[pb->received_n++];
		strcpy(d->server_id, r->server_id); /* lengths validated above */
		strcpy(d->username, r->username);
		d->verified = r->verified;
		d->addr_len = r->client_addr.len;
		if (d->addr_len)
			memcpy(d->addr, r->client_addr.data, d->addr_len);
	}

	if (invalid) {
		is2_received_count(&is2_received_invalid, invalid);
		hlog_packet(LOG_DEBUG, pb->data, pb->packet_len - 2, "%s/%s: IS2: dropped %d invalid Received entries of packet: ",
			c->addr_rem, c->username, invalid);
	}

	if (loop) {
		is2_received_count(&is2_received_loops, 1);
		hlog_packet(LOG_DEBUG, pb->data, pb->packet_len - 2, "%s/%s: IS2: packet has been received by this server before, %d Received entries: ",
			c->addr_rem, c->username, (int)in->n_received);
	}
}

/*
 *	Append the Received entry of this server to a packet: it was received
 *	from client c. The address of the client is recorded in it unless
 *	disabled in the configuration, as on an aggregator which only has
 *	other servers connected to it.
 */

static void is2_received_append(struct pbuf_t *pb, struct client_t *c)
{
	struct is2_received_t *d;
	int l;

	is2_received_make_room(pb);
	d = &pb->received[pb->received_n++];

	/* serverid and the usernames are validated to CALLSIGNLEN_MAX when set */
	l = strnlen(serverid, IS2_RECEIVED_ID_LEN);
	memcpy(d->server_id, serverid, l);
	d->server_id[l] = 0;
	l = strnlen(c->username, IS2_RECEIVED_ID_LEN);
	memcpy(d->username, c->username, l);
	d->username[l] = 0;
	d->verified = c->validated; /* VALIDATED_* values match the VerificationStatus enum */
	d->addr_len = 0;

	if (!is2_received_addr)
		return;

	/* the pseudo-clients of HTTP and UDP uploads have no address */
	if (c->addr.sa.sa_family == AF_INET) {
		memcpy(d->addr, &c->addr.si.sin_addr, 4);
		d->addr_len = 4;
	} else if (c->addr.sa.sa_family == AF_INET6) {
		if (IN6_IS_ADDR_V4MAPPED(&c->addr.si6.sin6_addr)) {
			/* an IPv4 client of a listener bound to :: */
			memcpy(d->addr, &c->addr.si6.sin6_addr.s6_addr[12], 4);
			d->addr_len = 4;
		} else {
			memcpy(d->addr, &c->addr.si6.sin6_addr, 16);
			d->addr_len = 16;
		}
	}
}

/*
 *	Check the received_time of an incoming IS2 packet against our clock.
 *	A packet which has been in the network for more than
 *	IS2_RECEIVED_MAX_AGE seconds has looped, or come through a server
 *	whose clock is off: it is counted, and dropped if IS2ReceivedAgeDrop
 *	is enabled. Returns 0 to accept the packet, or the INERR_* code to
 *	drop it with.
 */

int is2_received_age_check(struct client_t *c, uint64_t received_time)
{
	int64_t age;
	int e;

	/* A stamp beyond the signed range would wrap negative in the
	 * subtraction below: it is from the far future.
	 */
	if (received_time > (uint64_t)INT64_MAX)
		age = INT64_MIN;
	else
		age = (int64_t)now * 1000 - (int64_t)received_time;

	if (age > IS2_RECEIVED_MAX_AGE * 1000) {
		is2_received_count(&is2_received_too_old, 1);
		e = INERR_IS2_TOO_OLD;
	} else if (age < -IS2_RECEIVED_MAX_FUTURE * 1000) {
		is2_received_count(&is2_received_too_new, 1);
		e = INERR_IS2_TOO_NEW;
	} else {
		return 0;
	}

	/* A log line per link now and then. When the packet comes from an
	 * uplink or a peer, the clock of a server is likely off, and that is
	 * worth a warning. A client may send whatever stamps it likes, and
	 * only loses its own packets.
	 */
	if (tick >= c->is2_received_age_warn) {
		int level = client_is_server_link(c) ? LOG_WARNING : LOG_INFO;
		/* negated as unsigned, since -INT64_MIN does not fit */
		uint64_t mag = (age < 0) ? (uint64_t)0 - (uint64_t)age : (uint64_t)age;
		c->is2_received_age_warn = tick + IS2_RECEIVED_AGE_WARN_INTERVAL;
		hlog(level, "%s/%s: IS2: packet received_time is %llu seconds in the %s%s",
			c->addr_rem, c->username, (unsigned long long)(mag / 1000),
			(age < 0) ? "future" : "past",
			(is2_received_age_drop) ? ", dropping such packets" : "");
	}

	return (is2_received_age_drop) ? e : 0;
}
#endif /* USE_IS2 */

/*
 *	Prepare a pbuf to have an IS2 formatted packet, when handling an incoming packet
 */

void is2_pbuf_copy_additional(Aprsis2__ISPacket *dest, Aprsis2__ISPacket *src)
{
	if (src->optional_rx_rssi_dbm_case == APRSIS2__ISPACKET__OPTIONAL_RX_RSSI_DBM_RX_RSSI_DBM) {
		dest->optional_rx_rssi_dbm_case = APRSIS2__ISPACKET__OPTIONAL_RX_RSSI_DBM_RX_RSSI_DBM;
		dest->rx_rssi_dbm = src->rx_rssi_dbm;
	}
	if (src->optional_rx_snr_db_case == APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB_RX_SNR_DB) {
		dest->optional_rx_snr_db_case = APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB_RX_SNR_DB;
		dest->rx_snr_db = src->rx_snr_db;
	}
	if (src->rx_modem != APRSIS2__ISPACKET__MODEM_TYPE__MODEM_UNDEFINED)
		dest->rx_modem = src->rx_modem;
	if (src->optional_rx_bitrate_case == APRSIS2__ISPACKET__OPTIONAL_RX_BITRATE_RX_BITRATE) {
		dest->optional_rx_bitrate_case = APRSIS2__ISPACKET__OPTIONAL_RX_BITRATE_RX_BITRATE;
		dest->rx_bitrate = src->rx_bitrate;
	}
	if (src->optional_rx_frequency_case == APRSIS2__ISPACKET__OPTIONAL_RX_FREQUENCY_RX_FREQUENCY) {
		dest->optional_rx_frequency_case = APRSIS2__ISPACKET__OPTIONAL_RX_FREQUENCY_RX_FREQUENCY;
		dest->rx_frequency = src->rx_frequency;
	}
}

void is2_pbuf_init_packet(struct pbuf_t *pb, struct client_t *c, Aprsis2__ISPacket *is2_packet_in, Aprsis2__ISPacket *is2_input_template)
{
	aprsis2__ispacket__init(&pb->is2packet);
	pb->is2packet.type = APRSIS2__ISPACKET__TYPE__IS_PACKET;
	pb->is2packet.is_packet_data.data = (uint8_t *)pb->data;
	pb->is2packet.is_packet_data.len = pb->packet_len - 2; /* skip CR/LF */
	
	// Copy additional fields of the incoming IS2 packet to the new pbuf
	if (is2_packet_in) {
		is2_pbuf_copy_additional(&pb->is2packet, is2_packet_in);
	} else if (is2_input_template) {
		is2_pbuf_copy_additional(&pb->is2packet, is2_input_template);
	}

#ifdef USE_IS2
	/* The Received entries: the ones the packet came with, and ours.
	 * The pbuf has them in its own compact form; is2packet.received is
	 * filled in when the packet is serialized.
	 */
	pb->received_n = 0;
	if (is2_packet_in) {
		if (is2_packet_in->n_received > 0)
			is2_received_copy_in(pb, c, is2_packet_in);
		pb->is2packet.received_time = is2_packet_in->received_time;
	}
	if (is2_received)
		is2_received_append(pb, c);

	/* the first server to receive the packet stamps it with the time */
	if (pb->is2packet.received_time == 0)
		pb->is2packet.received_time = is2_time_ms();
#endif
}

/*
 *	Serialize a packet to a single element of the repeated is_packet field
 *	of an IS2Message: the field tag, the length of the ISPacket, and the
 *	ISPacket itself as packed by protobuf-c.
 *	Returns the length of the element, or -1 if it does not fit in buf.
 */

static int is2_encode_packet_element(char *buf, int buflen, Aprsis2__ISPacket *p)
{
	size_t plen = aprsis2__ispacket__get_packed_size(p);
	int hlen = pb_varint_len((IS2_MESSAGE_FIELD_IS_PACKET << 3) | PB_WIRETYPE_LEN) + pb_varint_len(plen);
	int i;

	if (hlen + plen > buflen)
		return -1;

	i = pb_put_tag(buf, IS2_MESSAGE_FIELD_IS_PACKET, PB_WIRETYPE_LEN);
	i += pb_put_varint(buf + i, plen);
	aprsis2__ispacket__pack(p, (uint8_t *)buf + i);

	return i + plen;
}

/*
 *	Are the addresses in the Received entries of packets removed before
 *	they are sent to this client? They are only passed on to the other
 *	servers: the uplinks and the peers.
 */

static int is2_received_redacted(struct client_t *c)
{
	return !client_is_server_link(c);
}

/*
 *	Encode the packet being distributed by the worker, unless the worker's
 *	encoding cache already has it: a packet going to many IS2 clients is
 *	serialized only once per worker, in each of the variants needed. The
 *	cache is invalidated by process_outgoing_single() when it moves to the
 *	next packet.
 */

static int is2_worker_encode_packet(struct worker_t *self, struct pbuf_t *pb, int variant)
{
	if (self->is2_enc_pb[variant] == pb)
		return 0;

#ifdef USE_IS2
	/* The Received entries are serialized from the pbuf's compact copies
	 * of them: a shallow copy of the packet, with the entries pointing to
	 * the pbuf, and without the addresses in the redacted variant.
	 */
	Aprsis2__ISPacket p = pb->is2packet;
	Aprsis2__IS2Received rcv[IS2_RECEIVED_MAX];
	Aprsis2__IS2Received *rcvp[IS2_RECEIVED_MAX];
	int i;

	for (i = 0; i < pb->received_n; i++) {
		aprsis2__is2_received__init(&rcv[i]);
		rcv[i].server_id = pb->received[i].server_id;
		rcv[i].username = pb->received[i].username;
		rcv[i].verified = pb->received[i].verified;
		if (variant == IS2_ENC_FULL && pb->received[i].addr_len) {
			rcv[i].client_addr.len = pb->received[i].addr_len;
			rcv[i].client_addr.data = pb->received[i].addr;
		}
		rcvp[i] = &rcv[i];
	}
	p.n_received = pb->received_n;
	p.received = rcvp;

	self->is2_enc_len[variant] = is2_encode_packet_element(self->is2_enc[variant], sizeof(self->is2_enc[variant]), &p);
#else
	self->is2_enc_len[variant] = is2_encode_packet_element(self->is2_enc[variant], sizeof(self->is2_enc[variant]), &pb->is2packet);
#endif
	if (self->is2_enc_len[variant] < 0) {
		hlog(LOG_ERR, "worker %d: IS2: encoded packet does not fit in %d bytes: %.*s",
			self->id, (int)sizeof(self->is2_enc[variant]), pb->packet_len - 2, pb->data);
		self->is2_enc_pb[variant] = NULL;
		return -1;
	}

	self->is2_enc_pb[variant] = pb;

	return 0;
}

/*
 *	Finish the frame in the IS2 output buffer: fill in the frame header
 *	and append the ETX. Returns the length of the complete frame.
 */

static int is2_obuf_close_frame(struct client_t *c)
{
	int len = c->is2_obuf_end - IS2_HEAD_LEN;

	is2_setup_buffer(c->is2_obuf, len);

	return IS2_HEAD_LEN + len + IS2_TAIL_LEN;
}

/*
 *	Flush the IS2 output buffer to a TCP (or SCTP) client.
 *	Returns the result of c->write: a value < -2 means the client was destroyed.
 */

int is2_obuf_flush(struct worker_t *self, struct client_t *c)
{
	int len, ret;

	if (c->is2_obuf_packets == 0)
		return 0;

	//hlog(LOG_DEBUG, "%s/%s: IS2: flushing obuf of %d packets, %d bytes", c->addr_rem, c->username, c->is2_obuf_packets, c->is2_obuf_total_len);

	len = is2_obuf_close_frame(c);

	/* c->write copies the frame to the socket output buffer of the client */
	ret = c->write(self, c, c->is2_obuf, len);
	if (ret < -2)
		return ret; // destroyed

	is2_obuf_reset(c);

	return ret;
}

/*
 *	Flush the IS2 output buffer to an UDP peer: append the sequence number,
 *	sign the frame and transmit it.
 */

int is2_corepeer_obuf_flush(struct worker_t *self, struct client_t *c)
{
	int len, signed_len, ret;
	uint32_t seq;

	if (c->is2_obuf_packets == 0)
		return 0;

	//hlog(LOG_DEBUG, "%s/%s: IS2 corepeer: flushing obuf of %d packets, %d bytes", c->addr_rem, c->username, c->is2_obuf_packets, c->is2_obuf_total_len);

	/* like protobuf-c, do not transmit the default value of 0 (proto3) */
	seq = c->corepeer_is2_sequence++;
	if (seq) {
		c->is2_obuf_end += pb_put_tag(c->is2_obuf + c->is2_obuf_end, IS2_MESSAGE_FIELD_SEQUENCE, PB_WIRETYPE_VARINT);
		c->is2_obuf_end += pb_put_varint(c->is2_obuf + c->is2_obuf_end, seq);
	}

	len = is2_obuf_close_frame(c);

	signed_len = is2_corepeer_message_sign(c, c->is2_obuf, IS2_OBUF_SIZE, len);
	if (signed_len < IS2_MINIMUM_FRAME_LEN) {
		is2_obuf_reset(c);
		return -1;
	}

	ret = udp_client_write(self, c, c->is2_obuf, signed_len);

	is2_obuf_reset(c);

	return ret;
}

static int is2_obuf_flush_client(struct worker_t *self, struct client_t *c)
{
	if (c->state == CSTATE_COREPEER)
		return is2_corepeer_obuf_flush(self, c);

	return is2_obuf_flush(self, c);
}

/*
 *	Append a packet to the IS2 output buffer of a client. The buffer is
 *	flushed first if the packet would not fit in it; otherwise the packet
 *	waits there for the rest of this round's packets, and the worker
 *	flushes the buffer at the end of the round (is2_obuf_flush_pending).
 *	Returns a value < -2 if the client was destroyed.
 */

static int is2_append_obuf_packet(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	int ret;
	int variant = (is2_received_redacted(c)) ? IS2_ENC_REDACTED : IS2_ENC_FULL;
	int len;

	if (is2_worker_encode_packet(self, pb, variant) < 0)
		return 0; // can not be encoded, drop
	len = self->is2_enc_len[variant];

	//hlog(LOG_DEBUG, "%s/%s: IS2: appending IS packet type %d of %d bytes to obuf", c->addr_rem, c->username, pb->is2packet.type, len);

	if (c->is2_obuf_packets >= APRSIS2_OBUF_PACKETS
	    || c->is2_obuf_total_len + len > APRSIS2_OBUF_MAX_LENGTH) {
		ret = is2_obuf_flush_client(self, c);
		if (ret < -2)
			return ret; // destroyed
	}

	/* copy the serialized packet to the client's own buffer - after this,
	 * the pbuf is not referred to any more
	 */
	memcpy(c->is2_obuf + c->is2_obuf_end, self->is2_enc[variant], len);
	c->is2_obuf_end += len;
	c->is2_obuf_total_len += len;
	c->is2_obuf_packets++;
	self->is2_obuf_pending = 1;

	return 0;
}

/*
 *	Flush the IS2 output buffers which were left with packets in this
 *	round of the worker, so that the packets which came in the round go
 *	out together in a frame, without waiting for the buffer to fill up or
 *	for the keepalive round, which flushes them as a last resort. The
 *	plaintext clients' buffers go out at the end of the round as well,
 *	when their sockets poll writable.
 */

void is2_obuf_flush_pending(struct worker_t *self)
{
	struct client_t *c, *cnext;

	if (!self->is2_obuf_pending)
		return;
	self->is2_obuf_pending = 0;

	for (c = self->clients; (c); c = cnext) {
		// the flush may destroy the client
		cnext = c->next;
		if (c->is2_obuf_packets)
			is2_obuf_flush_client(self, c);
	}
}

/*
 *	Write a single packet to an IS2 client, or to an IS2 UDP peer
 */

int is2_write_packet(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	return is2_append_obuf_packet(self, c, pb);
}

int is2_corepeer_write_packet(struct worker_t *self, struct client_t *c, struct pbuf_t *pb)
{
	return is2_append_obuf_packet(self, c, pb);
}

/*
 *	Verify at startup that the IS2Message envelope written by the output
 *	buffer code matches what protobuf-c generates for the same message,
 *	so that a change of field numbers in aprsis2.proto can not silently
 *	break the output path. Returns 0 when OK, -1 on mismatch.
 */

int is2_wire_selfcheck(void)
{
	char ref[APRSIS2_ENC_MAX + 32];
	char mine[APRSIS2_ENC_MAX + 32];
	int rlen, mlen, elen;

	/* an ISPacket with all the fields set */
	Aprsis2__ISPacket p = APRSIS2__ISPACKET__INIT;
	Aprsis2__ISPacket *pp[1] = { &p };
	p.type = APRSIS2__ISPACKET__TYPE__IS_PACKET_DUPLICATE;
	p.is_packet_data.data = (uint8_t *)"N0CALL>APRS,qAR,TEST:>IS2 wire format self-check";
	p.is_packet_data.len = strlen((char *)p.is_packet_data.data);
	p.optional_rx_rssi_dbm_case = APRSIS2__ISPACKET__OPTIONAL_RX_RSSI_DBM_RX_RSSI_DBM;
	p.rx_rssi_dbm = -101.5;
	p.optional_rx_snr_db_case = APRSIS2__ISPACKET__OPTIONAL_RX_SNR_DB_RX_SNR_DB;
	p.rx_snr_db = 7.25;
	Aprsis2__IS2Received rcv = APRSIS2__IS2_RECEIVED__INIT;
	Aprsis2__IS2Received *rcvp[1] = { &rcv };
	rcv.server_id = "TESTSRV";
	rcv.username = "N0CALL-15";
	rcv.verified = APRSIS2__VERIFICATION_STATUS__STRONG;
	rcv.client_addr.data = (uint8_t *)"\x7f\x00\x00\x01";
	rcv.client_addr.len = 4;
	p.n_received = 1;
	p.received = rcvp;
	p.received_time = 1700000000123ULL;

	/* message type and one packet, as flushed to a TCP client */
	Aprsis2__IS2Message m = APRSIS2__IS2_MESSAGE__INIT;
	m.type = APRSIS2__IS2_MESSAGE__TYPE__IS_PACKET;
	m.n_is_packet = 1;
	m.is_packet = pp;
	rlen = aprsis2__is2_message__pack(&m, (uint8_t *)ref);

	mlen = pb_put_tag(mine, IS2_MESSAGE_FIELD_TYPE, PB_WIRETYPE_VARINT);
	mlen += pb_put_varint(mine + mlen, APRSIS2__IS2_MESSAGE__TYPE__IS_PACKET);
	elen = is2_encode_packet_element(mine + mlen, sizeof(mine) - mlen, &p);
	if (elen < 0) {
		hlog(LOG_CRIT, "IS2: wire format self-check: packet element does not fit in buffer");
		return -1;
	}
	mlen += elen;

	if (rlen != mlen || memcmp(ref, mine, rlen) != 0) {
		hlog(LOG_CRIT, "IS2: wire format self-check failed for packet message (%d/%d bytes): aprsis2.proto and aprsis2.c disagree", rlen, mlen);
		return -1;
	}

	/* sequence number, as appended when flushing to an UDP peer */
	Aprsis2__IS2Message ms = APRSIS2__IS2_MESSAGE__INIT;
	ms.sequence = 0x12345678;
	rlen = aprsis2__is2_message__pack(&ms, (uint8_t *)ref);

	mlen = pb_put_tag(mine, IS2_MESSAGE_FIELD_SEQUENCE, PB_WIRETYPE_VARINT);
	mlen += pb_put_varint(mine + mlen, ms.sequence);

	if (rlen != mlen || memcmp(ref, mine, rlen) != 0) {
		hlog(LOG_CRIT, "IS2: wire format self-check failed for sequence number (%d/%d bytes): aprsis2.proto and aprsis2.c disagree", rlen, mlen);
		return -1;
	}

	return 0;
}


#define IS2_COREPEER_PROPOSAL_CHALLENGE_LEN 10
#define IS2_COREPEER_TOKEN_MAX_LEN 64

/* the range of UDP peering protocol versions supported; version 1 is the
 * plain UDP peering without a handshake */
#define IS2_COREPEER_VERSION_MIN 2
#define IS2_COREPEER_VERSION_MAX 2

/* the handshake lines end in " mac=" and an HMAC-SHA256 in hex */
#define IS2_COREPEER_MAC_HEX_LEN 64
#define IS2_COREPEER_MAC_SUFFIX_LEN (5 + IS2_COREPEER_MAC_HEX_LEN)

/* the fields of a "#is2 peer" handshake line, not NUL-terminated */
struct is2_corepeer_control_t {
	const char *v, *id, *vers, *token, *key, *ts, *mac;
	int v_len, id_len, vers_len, token_len, key_len, ts_len, mac_len;
};

static void is2_corepeer_control_key(void *ctx, const char *key, int key_len, const char *val, int val_len)
{
	struct is2_corepeer_control_t *ctl = ctx;

	if (key_len == 1 && memcmp(key, "v", 1) == 0) {
		ctl->v = val;
		ctl->v_len = val_len;
	} else if (key_len == 2 && memcmp(key, "id", 2) == 0) {
		ctl->id = val;
		ctl->id_len = val_len;
	} else if (key_len == 4 && memcmp(key, "vers", 4) == 0) {
		ctl->vers = val;
		ctl->vers_len = val_len;
	} else if (key_len == 5 && memcmp(key, "token", 5) == 0) {
		ctl->token = val;
		ctl->token_len = val_len;
	} else if (key_len == 3 && memcmp(key, "key", 3) == 0) {
		ctl->key = val;
		ctl->key_len = val_len;
	} else if (key_len == 2 && memcmp(key, "ts", 2) == 0) {
		ctl->ts = val;
		ctl->ts_len = val_len;
	} else if (key_len == 3 && memcmp(key, "mac", 3) == 0) {
		ctl->mac = val;
		ctl->mac_len = val_len;
	}
	/* unknown keys are ignored, so that later versions can add them */
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 *	Send a "#is2 peer" handshake line: a hello offering the versions from
 *	v_min to v_max, or an ok with the one picked (v_min == v_max).
 */
static int is2_corepeer_control_out(struct worker_t *self, struct client_t *c, const char *type,
	int v_min, int v_max, const char *token, int token_len)
{
	unsigned int key_id = peergroup_key_transmit;
	char vlist[64];
	char vers[64];
	char buf[512];
	int vl = 0;

	for (int v = v_min; v <= v_max && vl < sizeof(vlist) - 12; v++)
		vl += snprintf(vlist + vl, sizeof(vlist) - vl, "%s%d", (vl) ? "," : "", v);

	/* the software name and version as one value, without spaces */
	snprintf(vers, sizeof(vers), "%s/%s", verstr_progname, version_build);
	for (char *s = vers; *s; s++)
		if (*s == ' ' || *s == '=')
			*s = '_';

	/* the signature goes last: it covers everything before " mac=" */
	int l = snprintf(buf, sizeof(buf), IS2_COREPEER_CONTROL_PREFIX "%s v=%s id=%s vers=%s token=%.*s key=%u ts=%u",
		type, vlist, serverid, vers, token_len, token, key_id, (uint32_t)now);

	/* room for the signature, the CRLF and the terminating NUL */
	if (l + IS2_COREPEER_MAC_SUFFIX_LEN + 2 + 1 > sizeof(buf)) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: handshake line too long", c->addr_rem, c->username);
		return -1;
	}

	unsigned char md[32];
	unsigned int md_len = 0;
	int r = is2_corepeer_hmac(key_id, buf, l, md, &md_len);
	if (r != 0 || md_len != sizeof(md)) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: HMAC calculation failed for handshake (key %u)", c->addr_rem, c->username, key_id);
		return -1;
	}

	l += snprintf(buf + l, sizeof(buf) - l, " mac=");
	for (int i = 0; i < md_len; i++)
		l += snprintf(buf + l, sizeof(buf) - l, "%02x", md[i]);
	l += snprintf(buf + l, sizeof(buf) - l, "\r\n");

	return c->write(self, c, buf, l);
}

/*
 *	Check the signature of a "#is2 peer" handshake line, which must end in
 *	" mac=<64 hex digits>", an HMAC-SHA256 of everything before it, with
 *	the key given in the key= field, and the time in ts= within the same
 *	window as for the data frames. Returns 0 if the line is authentic.
 */
static int is2_corepeer_control_verify(struct client_t *c, const char *p, int len,
	struct is2_corepeer_control_t *ctl)
{
	int signed_len = len - IS2_COREPEER_MAC_SUFFIX_LEN;
	long long key_id, ts;

	if (signed_len <= 0 || memcmp(p + signed_len, " mac=", 5) != 0
	    || ctl->mac != p + signed_len + 5 || ctl->mac_len != IS2_COREPEER_MAC_HEX_LEN) {
		hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP: control packet without a signature at the end: ", c->addr_rem, c->username);
		return -1;
	}

	if (!ctl->key || !is2_kv_int(ctl->key, ctl->key_len, 0, IS2_PEERGROUP_KEY_ID_NUM-1, &key_id)
	    || !ctl->ts || !is2_kv_int(ctl->ts, ctl->ts_len, 0, UINT32_MAX, &ts)) {
		hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP: control packet signature without a valid key or ts: ", c->addr_rem, c->username);
		return -1;
	}

	unsigned char md[32];
	unsigned int md_len = 0;
	int r = is2_corepeer_hmac(key_id, p, signed_len, md, &md_len);
	if (r == IS2_HMAC_NO_KEY) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: control packet signed with undefined key ID %lld", c->addr_rem, c->username, key_id);
		return -1;
	}
	if (r != 0 || md_len != sizeof(md)) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: HMAC calculation failed (key %lld)", c->addr_rem, c->username, key_id);
		return -1;
	}

	/* the received MAC in hex, both cases accepted */
	unsigned char rmd[32];
	for (int i = 0; i < sizeof(rmd); i++) {
		int hi = hexval(ctl->mac[i*2]);
		int lo = hexval(ctl->mac[i*2+1]);
		if (hi < 0 || lo < 0) {
			hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP: control packet with an invalid mac: ", c->addr_rem, c->username);
			return -1;
		}
		rmd[i] = hi << 4 | lo;
	}

	if (CRYPTO_memcmp(md, rmd, sizeof(md)) != 0) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: control packet with wrong signature (key_id %lld)", c->addr_rem, c->username, key_id);
		return -1;
	}

	int64_t age = (int64_t)(uint32_t)now - (int64_t)ts;
	if (age > IS2_UDP_FRAME_MAX_AGE || age < -IS2_UDP_FRAME_MAX_AGE) {
		hlog(LOG_WARNING, "%s/%s: IS2 UDP: control packet timestamp is off by %lld seconds, dropping (clocks not in sync, or a replayed packet)",
			c->addr_rem, c->username, (long long)age);
		return -1;
	}

	return 0;
}

int is2_corepeer_propose(struct worker_t *self, struct client_t *c)
{
	hlog(LOG_DEBUG, "%s/%s: IS2 UDP: control: proposing IS2", c->addr_rem, c->username);
	
	if (!c->corepeer_is2_challenge)
		c->corepeer_is2_challenge = hmalloc(IS2_COREPEER_PROPOSAL_CHALLENGE_LEN+1);
	
	urandom_token((unsigned char *)c->corepeer_is2_challenge, IS2_COREPEER_PROPOSAL_CHALLENGE_LEN+1);

	return is2_corepeer_control_out(self, c, "hello", IS2_COREPEER_VERSION_MIN, IS2_COREPEER_VERSION_MAX,
		c->corepeer_is2_challenge, strlen(c->corepeer_is2_challenge));
}

typedef enum {
        IS2_COREPEER_CONTROL_UNDEF,
        IS2_COREPEER_CONTROL_HELLO,
        IS2_COREPEER_CONTROL_OK
} IS2CorepeerControlEnum;

/*
 *	The token is echoed back in the reply: only take printable
 *	characters of a sane length.
 */
static int is2_corepeer_token_valid(const char *token, int token_len)
{
	if (token_len < 1 || token_len > IS2_COREPEER_TOKEN_MAX_LEN)
		return 0;

	for (int i = 0; i < token_len; i++)
		if (token[i] <= ' ' || token[i] > '~')
			return 0;

	return 1;
}

/*
 *	Handle a "#is2 peer" handshake datagram from a UDP peer:
 *
 *	#is2 peer hello v=2[,3...] id=SERVERID vers=aprsc/2.1.20 token=abc123 [key=value ...] key=1 ts=1760000000 mac=<hex>
 *	#is2 peer ok v=2 id=SERVERID vers=aprsc/2.1.20 token=abc123 [key=value ...] key=1 ts=1760000000 mac=<hex>
 *
 *	The keys may come in any order, and unknown ones are ignored, but the
 *	mac must be the last one: it signs everything before it.
 */
int is2_corepeer_control_in(struct worker_t *self, struct client_t *c, char *p, int len)
{
	IS2CorepeerControlEnum type = IS2_COREPEER_CONTROL_UNDEF;
	struct is2_corepeer_control_t ctl;

	/* a line terminator is optional */
	while (len > 0 && (p[len-1] == '\r' || p[len-1] == '\n'))
		len--;

	hlog_packet(LOG_DEBUG, p, len, "%s/%s: IS2 UDP: control packet in: ", c->addr_rem, c->username);

	/* without a key to sign frames with, the link stays on plain UDP */
	if (!is2_corepeer_enabled()) {
		hlog_packet(LOG_DEBUG, p, len, "%s/%s: IS2 UDP: control packet ignored, no PeerGroupKey configured: ", c->addr_rem, c->username);
		return 0;
	}

	/* the caller has checked for the "#is2 peer " prefix */
	const char *s = p + IS2_COREPEER_CONTROL_PREFIX_LEN;
	int slen = len - IS2_COREPEER_CONTROL_PREFIX_LEN;
	int type_len = 0;

	while (type_len < slen && s[type_len] != ' ')
		type_len++;

	if (type_len == 5 && memcmp(s, "hello", 5) == 0) {
		type = IS2_COREPEER_CONTROL_HELLO;
	} else if (type_len == 2 && memcmp(s, "ok", 2) == 0) {
		type = IS2_COREPEER_CONTROL_OK;
	} else {
		hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP: Invalid control packet, unknown type: ", c->addr_rem, c->username);
		return -1;
	}

	memset(&ctl, 0, sizeof(ctl));
	is2_kv_parse(s + type_len, slen - type_len, is2_corepeer_control_key, &ctl);

	/* only trust a handshake signed with one of our peer group keys, so
	 * that it can not be spoofed, and IS2 is only enabled with a peer
	 * which has a common key */
	if (is2_corepeer_control_verify(c, p, len, &ctl) != 0)
		return -1;

	if (!ctl.v || !ctl.id || !ctl.token) {
		hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP: Invalid control packet, v, id or token missing: ", c->addr_rem, c->username);
		return -1;
	}

	if (ctl.id_len != c->username_len || memcmp(ctl.id, c->username, ctl.id_len) != 0) {
		hlog(LOG_ERR, "%s/%s: IS2 UDP: PeerGroup config peer ID mismatch with ID reported by peer '%.*s'",
			c->addr_rem, c->username, ctl.id_len, ctl.id);
		return -1;
	}

	if (!is2_corepeer_token_valid(ctl.token, ctl.token_len)) {
		hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP: Invalid control packet, bad token: ", c->addr_rem, c->username);
		return -1;
	}

	if (type == IS2_COREPEER_CONTROL_HELLO) {
		/* the peer asks us whether we can do IS2 peering, and which
		 * versions it can do: pick the highest common one */
		int v = is2_kv_version_pick(ctl.v, ctl.v_len, IS2_COREPEER_VERSION_MIN, IS2_COREPEER_VERSION_MAX);

		if (!v) {
			/* no reply: the link stays on plain UDP, as with a peer
			 * which does not do IS2 at all */
			hlog(LOG_INFO, "%s/%s: IS2 UDP: peer proposed IS2 versions %.*s, none of which are supported",
				c->addr_rem, c->username, ctl.v_len, ctl.v);
			return 0;
		}

		hlog(LOG_DEBUG, "%s/%s: IS2 UDP: control: peer proposed IS2, responding OK with v=%d", c->addr_rem, c->username, v);

		if (!c->corepeer_is2) {
			// If we're not yet in IS2 mode ourself, offer again soon
			c->next_is2_peer_offer = tick + 1;
		} else {
			/* The peer restarted, or fell back and comes back, or
			 * proposes again on a working link, as it does every
			 * ten minutes. The signed proposal is a sign of life:
			 * start the fallback timer over, so that it does not
			 * count from before an outage of the peer and fire
			 * right after it has recovered. The round-trip times
			 * and the frame loss window are kept: on a working
			 * link they would be lost on every proposal. */
			c->ping_reply_tick = tick;
		}

		return is2_corepeer_control_out(self, c, "ok", v, v, ctl.token, ctl.token_len);
	}

	if (type == IS2_COREPEER_CONTROL_OK) {
		/* the peer replies "I can do IS2", with the version it picked */
		if (!c->corepeer_is2_challenge) {
			hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP control 'ok' packet: no challenge stored, no 'hello' sent: ", c->addr_rem, c->username);
			return -1;
		}

		if (ctl.token_len != strlen(c->corepeer_is2_challenge)
		    || memcmp(ctl.token, c->corepeer_is2_challenge, ctl.token_len) != 0) {
			hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP control 'ok' packet: challenge mismatch: ", c->addr_rem, c->username);
			return -1;
		}

		/* exactly one version, which we offered */
		int v = is2_kv_version_pick(ctl.v, ctl.v_len, IS2_COREPEER_VERSION_MIN, IS2_COREPEER_VERSION_MAX);
		if (!v || memchr(ctl.v, ',', ctl.v_len)) {
			hlog_packet(LOG_WARNING, p, len, "%s/%s: IS2 UDP control 'ok' packet: invalid version: ", c->addr_rem, c->username);
			return -1;
		}

		/* OK, enable */
		c->corepeer_is2 = 1;
		c->write_packet = is2_corepeer_write_packet;
		c->is2_input_handler = is2_input_handler_corepeer;
		is2_corepeer_track_reset(c);
		/* and start pinging right away */
		c->keepalive = tick;

		/* Do send offers every 10 minutes away */
		c->next_is2_peer_offer = tick + COREPEER_IS2_PROPOSE_T_MAX + random() % 5;

		hlog(LOG_INFO, "%s/%s: IS2 UDP peer mode enabled, v=%d, peer software %.*s",
			c->addr_rem, c->username, v, (ctl.vers) ? ctl.vers_len : 1, (ctl.vers) ? ctl.vers : "-");
	}

	return 0;
}

static void is2_corepeer_key_free(struct is2_corepeer_key_t *k)
{
	if (!k)
		return;

	hfree(k->key);
	hfree(k);
}

/*
 *	Build the peer key table from the configuration and replace the
 *	previous one. Called from the main thread at startup and on reload;
 *	keys removed from the configuration are no longer accepted.
 */

int is2_corepeer_key_setup(void)
{
	struct is2_corepeer_key_t *new_keys[IS2_PEERGROUP_KEY_ID_NUM];
	struct is2_corepeer_key_t *old_keys[IS2_PEERGROUP_KEY_ID_NUM];
	struct peergroup_key_t *pk;
	struct is2_corepeer_key_t *k;
	int i;

	memset(new_keys, 0, sizeof(new_keys));

	for (pk = peergroup_key_config; (pk); pk = pk->next) {
		/* a key id defined twice: the later entry in the list wins */
		is2_corepeer_key_free(new_keys[pk->key_id]);

		k = hmalloc(sizeof(*k));
		k->key = (unsigned char *)hstrdup(pk->key);
		k->key_len = strlen(pk->key);
		new_keys[pk->key_id] = k;
	}

	rwl_wrlock(&is2_corepeer_keys_lock);
	memcpy(old_keys, is2_corepeer_keys, sizeof(old_keys));
	memcpy(is2_corepeer_keys, new_keys, sizeof(is2_corepeer_keys));
	rwl_wrunlock(&is2_corepeer_keys_lock);

	for (i = 0; i < IS2_PEERGROUP_KEY_ID_NUM; i++)
		is2_corepeer_key_free(old_keys[i]);

	/* IS2 peering is only done with a key to sign the frames with */
	if (peerip_config && !new_keys[peergroup_key_transmit]) {
		if (peergroup_key_config)
			hlog(LOG_WARNING, "IS2 UDP peering disabled: PeerGroupKeyTransmit %d has no PeerGroupKey", peergroup_key_transmit);
		else
			hlog(LOG_INFO, "IS2 UDP peering disabled: no PeerGroupKey configured");
	}

	return 0;
}

/*
 *	IS2 peering signs every frame with the PeerGroupKeyTransmit key, so it
 *	is only proposed to peers, and accepted from them, when that key has
 *	been configured. Without it the peer links stay on plain UDP.
 */

int is2_corepeer_enabled(void)
{
	int r;

	rwl_rdlock(&is2_corepeer_keys_lock);
	r = (is2_corepeer_keys[peergroup_key_transmit] != NULL);
	rwl_rdunlock(&is2_corepeer_keys_lock);

	return r;
}
