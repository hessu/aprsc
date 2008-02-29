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

