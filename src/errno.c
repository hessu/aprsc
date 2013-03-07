
/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

/*
 *	Error codes and their descriptions
 */

#include <string.h>

#include "errno.h"

const char *aprsc_errs[] = {
	"aprsc success",
	"Unknown error",
	"All peers being closed",
	"aprsc thread shutdown",
	"Client fd number invalid",
	"EOF - remote end closed connection",
	"Output buffer full",
	"Output write timeout",
	"Uplink server protocol error",
	"Uplink server says we're not verified",
	"Client login retry count exceeded",
	"Client login timed out",
	"Inactivity timeout",
	"Uplink server certificate validation failed"
};

const char *aprsc_strerror(int er)
{
	if (er >= 0)
		return strerror(er);
	
	er *= -1;
	
	if (er > APRSC_ERRNO_MAX)
		er = 1;
	
	return aprsc_errs[er];
}

