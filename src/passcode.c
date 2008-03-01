
#include <string.h>
#include <ctype.h>

#include "passcode.h"

/* As of April 11 2000 Steve Dimse has released this code to the open
 * source aprs community
 *
 * (from aprsd sources)
 */

#define kKey 0x73e2		// This is the key for the data

short aprs_passcode(const char* theCall)
{
	char rootCall[10];	// need to copy call to remove ssid from parse
	char *p1 = rootCall;
	
	while ((*theCall != '-') && (*theCall != 0) && (p1 < rootCall + 9))
		*p1++ = toupper(*theCall++);
	
	*p1 = 0;
	
	short hash = kKey;		// Initialize with the key value
	short i = 0;
	short len = strlen(rootCall);
	char *ptr = rootCall;
	
	while (i < len) {		// Loop through the string two bytes at a time
		hash ^= (*ptr++)<<8;	// xor high byte with accumulated hash
		hash ^= (*ptr++);	// xor low byte with accumulated hash
		i += 2;
	}
	
	return hash & 0x7fff;		// mask off the high bit so number is always positive
}

