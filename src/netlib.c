/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "netlib.h"

#if 0
/*
 *	Convert address & port to string
 */

int aptoa(struct in_addr sin_addr, int sin_port, char *s, int len)
{
	int l;
	
	if (sin_addr.s_addr == INADDR_ANY)
		return snprintf(s, len, "*:%d", sin_port);
	else {
		if (inet_ntop(AF_INET, (void *)&sin_addr, s, len) == NULL) {
			return snprintf(s, len, "ERROR:%d", sin_port);
		} else {
			l = strlen(s);
			return snprintf(s + l, len - l, ":%d", sin_port) + l;
		}
	}
}

/*
 *	Convert return values of gethostbyname() to a string
 */

int h_strerror(int i, char *s, int len)
{
	switch (i) {
		case HOST_NOT_FOUND:
			return snprintf(s, len, "Host not found");
		case NO_ADDRESS:
			return snprintf(s, len, "No IP address found for name");
		case NO_RECOVERY:
			return snprintf(s, len, "A non-recovable name server error occurred");
		case TRY_AGAIN:
			return snprintf(s, len, "A temporary error on an authoritative name server");
		default:
			return snprintf(s, len, "%d", i);
	}
}
#endif

