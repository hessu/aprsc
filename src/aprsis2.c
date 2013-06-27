
#include "hmalloc.h"
#include "worker.h"
#include "aprsis2.h"
#include "aprsis2.pb-c.h"
#include "version.h"
#include "hlog.h"

#define IS2_HEAD_LEN 4
#define IS2_TAIL_LEN 1

static void *is2_allocate_buffer(int len)
{
	int nlen = len + IS2_HEAD_LEN + IS2_TAIL_LEN;
	char *buf = hmalloc(nlen);
	uint32_t *ip = (uint32_t *)buf;
	
	*ip = htonl(len);
	
	buf[nlen-1] = '\n';
	
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
	
	sig.app_name = verstr_progname;
	sig.app_version = version_build;
	len = is2_message__get_packed_size(&m);
	buf = is2_allocate_buffer(len);
	is2_message__pack(&m, buf + IS2_HEAD_LEN);
	
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
	
	hlog(LOG_INFO, "%s/%s: IS2: Server signature received: app %s version %s",
		c->addr_rem, c->username, sig->app_name, sig->app_version);
	
	is2_message__free_unpacked(m, NULL);
	
	return 0;
}

#define IS2_MINIMUM_FRAME_LEN 4 + 1 + 1
#define IS2_MINIMUM_FRAME_CONTENT_LEN 4

static int is2_deframe(struct worker_t *self, struct client_t *c, char *s, int len)
{
	if (len < IS2_MINIMUM_FRAME_LEN) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Too short frame wrapper (%d): ",
			c->addr_rem, c->username, len);
		return -1;
	}
	
	uint32_t *ip = (uint32_t *)s;
	int clen = ntohl(*ip);
	
	if (clen < IS2_MINIMUM_FRAME_CONTENT_LEN) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Too short frame content (%d): ",
			c->addr_rem, c->username, clen);
		return -1;
	}
	
	if (IS2_HEAD_LEN + clen + IS2_TAIL_LEN > len) {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Frame length points behind buffer end (%d): ",
			c->addr_rem, c->username, clen);
		return -1;
	}
	
	if (s[IS2_HEAD_LEN + clen] != '\n') {
		hlog_packet(LOG_WARNING, s, len, "%s/%s: IS2: Frame missing terminating LF: ",
			c->addr_rem, c->username);
		return -1;
	}
	
	hlog_packet(LOG_DEBUG, s, len, "%s/%s: IS2: framing ok: ", c->addr_rem, c->username);
	
	is2_unpack_server_signature(self, c, s + IS2_HEAD_LEN, clen);
	
	return 0;
}

int is2_in_server_signature(struct worker_t *self, struct client_t *c, char *s, int len)
{
	/* this one comes through the CRLF deframing */
	is2_deframe(self, c, s, len+1);
	
	return 0;
}
