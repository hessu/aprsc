
#include "hmalloc.h"
#include "worker.h"
#include "aprsis2.h"
#include "aprsis2.pb-c.h"
#include "version.h"

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

int is2_data_in(struct worker_t *self, struct client_t *c)
{
	return 0;
}
