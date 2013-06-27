
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
	ServerSignature sig = SERVER_SIGNATURE__INIT;
	void *buf;	// Buffer to store serialized data
	unsigned len;	// Length of serialized data
	
	sig.app_name = verstr_progname;
	sig.app_version = version_build;
	len = server_signature__get_packed_size(&sig);
	buf = is2_allocate_buffer(len);
	server_signature__pack(&sig, buf + IS2_HEAD_LEN);
	
	c->write(self, c, buf, len + IS2_HEAD_LEN + IS2_TAIL_LEN);
	
	hfree(buf);
	
	return 0;
}

static int is2_unpack_server_signature(struct worker_t *self, struct client_t *c, void *buf, int len)
{
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
	
	is2_unpack_server_signature(self, c, s, len);
	
	return 0;
}

int is2_in_server_signature(struct worker_t *self, struct client_t *c, char *s, int len)
{
	/* this one comes through the CRLF deframing */
	is2_deframe(self, c, s, len+1);
	
	return 0;
}
