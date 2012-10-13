
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

#include "errno.h"

const char *aprsc_errs[] = {
	"Unknown error"
};

const char *aprsc_strerror(int errnum)
{
	if (errnum < 0)
		errnum *= -1;
	
	return "";
}

