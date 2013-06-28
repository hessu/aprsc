
#include "hmalloc.h"
#include "worker.h"
#include "aprsis2.h"
#include "aprsis2.pb-c.h"
#include "version.h"
#include "hlog.h"
#include "config.h"

#define STX 0x02
#define ETX 0x03

#define IS2_HEAD_LEN 4 /* STX + network byte order 24 bits uint to specify body length */
#define IS2_TAIL_LEN 1 /* ETX */

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

int is2_out_server_signature(struct worker_t *self, struct client_t *c)
{
	IS2Message m = IS2_MESSAGE__INIT;
	ServerSignature sig = SERVER_SIGNATURE__INIT;
	
	void *buf;	// Buffer to store serialized data
	unsigned len;	// Length of serialized data
	
	m.type = IS2_MESSAGE__TYPE__SERVER_SIGNATURE;
	m.serversignature = &sig;
	
	sig.username = serverid;
	sig.app_name = verstr_progname;
	sig.app_version = version_build;
	sig.n_features = 0;
	sig.features = NULL;
	len = is2_message__get_packed_size(&m);
	buf = is2_allocate_buffer(len);
	is2_message__pack(&m, buf + IS2_HEAD_LEN);
	
	//c->write(self, c, "#IS2\n", 5); /* not a good idea after all */
	c->write(self, c, buf, len + IS2_HEAD_LEN + IS2_TAIL_LEN);
	
	hfree(buf);
	
	return 0;
}

static int is2_unpack_server_signature(struct worker_t *self, struct client_t *c, void *buf, int len)
{
	IS2Message *m = is2_message__unpack(NULL, len, buf);
	if (!m) {
		hlog_packet(LOG_WARNING, buf, len, "%s/%s: IS2: unpacking of message failed: ",
			c->addr_rem, c->username);
		return 0;
	}
	
	ServerSignature *sig = m->serversignature;
	if (!sig) {
		hlog_packet(LOG_WARNING, buf, len, "%s/%s: IS2: unpacking of server signature failed: ",
			c->addr_rem, c->username);
		return 0;
	}
	
	hlog(LOG_INFO, "%s/%s: IS2: Server signature received: username %s app %s version %s",
		c->addr_rem, c->username, sig->username, sig->app_name, sig->app_version);
	
	is2_message__free_unpacked(m, NULL);
	
	return 0;
}


int is2_out_ping(struct worker_t *self, struct client_t *c)
{
	IS2Message m = IS2_MESSAGE__INIT;
	KeepalivePing ping = KEEPALIVE_PING__INIT;
	ProtobufCBinaryData rdata;
	
	rdata.data = NULL;
	rdata.len  = 0; 
	
	void *buf;	// Buffer to store serialized data
	unsigned len;	// Length of serialized data
	
	m.type = IS2_MESSAGE__TYPE__KEEPALIVE_PING;
	m.keepalive_ping = &ping;
	
	ping.ping_type = KEEPALIVE_PING__PING_TYPE__REQUEST;
	ping.request_id = random();
	ping.request_data = rdata;
	len = is2_message__get_packed_size(&m);
	buf = is2_allocate_buffer(len);
	is2_message__pack(&m, buf + IS2_HEAD_LEN);
	
	int r = c->write(self, c, buf, len + IS2_HEAD_LEN + IS2_TAIL_LEN);
	
	hfree(buf);
	
	return r;
}


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
			return -1;
		}
		
		if (this[IS2_HEAD_LEN + clen] != ETX) {
			hlog_packet(LOG_WARNING, this, left, "%s/%s: IS2: Frame missing terminating ETX: ",
				c->addr_rem, c->username);
			return -1;
		}
		
		hlog_packet(LOG_DEBUG, this, left, "%s/%s: IS2: framing ok: ", c->addr_rem, c->username);
		
		is2_unpack_server_signature(self, c, this + IS2_HEAD_LEN, clen);
		i += IS2_HEAD_LEN + clen + IS2_TAIL_LEN;
	}
	
	return i;
}

static int is2_deframe(struct worker_t *self, struct client_t *c, char *s, int len)
{
	if (len < IS2_MINIMUM_FRAME_LEN) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Too short frame wrapper (%d): ",
			c->addr_rem, c->username, len);
		return -1;
	}
	
	if (s[0] != STX) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Frame missing STX in beginning: ",
			c->addr_rem, c->username);
		return -1;
	}
	
	uint32_t *ip = (uint32_t *)s;
	uint32_t clen = ntohl(*ip) & 0xffffff;
	
	if (clen < IS2_MINIMUM_FRAME_CONTENT_LEN) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Too short frame content (%d): ",
			c->addr_rem, c->username, clen);
		return -1;
	}
	
	if (IS2_HEAD_LEN + clen + IS2_TAIL_LEN > len) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Frame length points behind buffer end (%d+%d buflen %d): ",
			c->addr_rem, c->username, clen, IS2_HEAD_LEN + IS2_TAIL_LEN, len);
		return -1;
	}
	
	if (s[IS2_HEAD_LEN + clen] != ETX) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Frame missing terminating ETX: ",
			c->addr_rem, c->username);
		return -1;
	}
	
	hlog_packet(LOG_DEBUG, s, len, "%s/%s: IS2: framing ok: ", c->addr_rem, c->username);
	
	is2_unpack_server_signature(self, c, s + IS2_HEAD_LEN, clen);
	
	return 0;
}

