
#include "hmalloc.h"
#include "worker.h"
#include "aprsis2.h"
#include "aprsis2.pb-c.h"
#include "version.h"

int is2_out_server_signature(struct worker_t *self, struct client_t *c)
{
	ServerSignature sig = SERVER_SIGNATURE__INIT;
	void *buf;	// Buffer to store serialized data
	unsigned len;	// Length of serialized data
	
	sig.app_name = verstr_progname;
	sig.app_version = version_build;
	len = server_signature__get_packed_size(&sig);
	buf = hmalloc(len);
	server_signature__pack(&sig, buf);
	
	c->write(self, c, buf, len);
	
	hfree(buf);
	
	return 0;
}

int is2_in_server_signature(struct worker_t *self, struct client_t *c)
{
	return 0;
}
