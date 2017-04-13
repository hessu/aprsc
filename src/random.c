
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include "random.h"
#include "hlog.h"

int urandom_open(void)
{
	int fd;
	
	if ((fd = open("/dev/urandom", O_RDONLY)) == -1) {
		hlog(LOG_ERR, "open(/dev/urandom) failed: %s", strerror(errno));
	}
	
	return fd;
}

int urandom_alphanumeric(int fd, unsigned char *buf, int buflen)
{
	int l;
	int len = buflen - 1;
	unsigned char c;
	
	if (fd >= 0) {
		/* generate instance id */
		l = read(fd, buf, len);
		if (l != len) {
			hlog(LOG_ERR, "read(/dev/urandom, %d) failed: %s", len, strerror(errno));
			close(fd);
			fd = -1;
		}
	}
	
	if (fd < 0) {
		/* urandom failed for us, use something inferior */
		for (l = 0; l < len; l++) {
			// coverity[dont_call]  // squelch warning: not security sensitive use of random()
			buf[l] = random() % 256;
		}
	}
	
	for (l = 0; l < len; l++) {
		/* 256 is not divisible by 36, the distribution is slightly skewed,
		 * but that's not serious.
		 */
		c = buf[l] % (26 + 10); /* letters and numbers */
		if (c < 10)
			c += 48; /* number */
		else
			c = c - 10 + 97; /* letter */
		buf[l] = c;
	}
	
	buf[len] = 0;
	
	return len;
}

