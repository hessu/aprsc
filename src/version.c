
#include "version.h"
#include "version_data.h"
#include "version_branch.h"
#include "config.h"
#include "xpoll.h"
#include "tls.h"

char verstr_progname[] = PROGNAME;
char version_build[] = VERSION "-" SRCVERSION VERSION_BRANCH;
const char verstr[]        = PROGNAME " " VERSION "-" SRCVERSION VERSION_BRANCH;
const char verstr_aprsis[] = PROGNAME " " VERSION "-" SRCVERSION VERSION_BRANCH;
const char verstr_http[] = PROGNAME "/" VERSION;

const char verstr_build_time[] = BUILD_TIME;
const char verstr_build_user[] = BUILD_USER;

const char verstr_features[] = 
#ifdef XP_USE_EPOLL
	" epoll"
#endif
#ifdef XP_USE_POLL
	" poll"
#endif
#ifdef USE_EVENTFD
	" eventfd"
#endif
#ifdef USE_POSIX_CAP
	" posix_cap"
#endif
#ifdef USE_CLOCK_GETTIME
	" clock_gettime"
#endif
#ifdef HAVE_SYNC_FETCH_AND_ADD
	" gcc_atomics"
#endif
#ifdef HAVE_LIBZ
	" zlib"
#endif
#ifdef _FOR_VALGRIND_
	" valgrind"
#endif
#ifdef USE_SSL
	" tls"
#endif
#ifdef USE_SCTP
	" sctp"
#endif
;
