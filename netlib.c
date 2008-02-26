
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "netlib.h"

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

