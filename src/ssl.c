
#include "config.h"
#include "ssl.h"
#include "hlog.h"

#ifdef USE_SSL

#include <openssl/conf.h>
#include <openssl/ssl.h>

int ssl_init(void)
{
	hlog(LOG_INFO, "Initializing OpenSSL...");
	
	OPENSSL_config(NULL);
	
	SSL_library_init();
	SSL_load_error_strings();
	
	OpenSSL_add_all_algorithms();
                
	return 0;
}

#endif

