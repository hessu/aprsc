/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
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
/*
	Libc fill-in for ZMailer using IPv6 API
	by Matti Aarnio <mea@nic.funet.fi> 1997, 2001

	The original Craig Metz code is deeply Linux specific,
	this adaptation tries to be way more generic..
*/

#include "../config.h"
#include <stdio.h>
#include <sys/types.h>
#include <netdb.h>
#ifndef EAI_BADFLAGS
# include "netdb6.h"
#endif

#ifndef __STDC__
# define const
#endif

#ifndef HAVE_GAI_STRERROR

/*
%%% copyright-cmetz-97
This software is Copyright 1997 by Craig Metz, All Rights Reserved.
The Inner Net License Version 2 applies to this software.
You should have received a copy of the license with this software. If
you didn't get a copy, you may request one from <license@inner.net>.

*/

const char *gai_strerror(errnum)
int errnum;
{
  static char buffer[24];
  switch(errnum) {
    case 0:
      return "no error";
    case EAI_BADFLAGS:
      return "invalid value for ai_flags";
    case EAI_NONAME:
      return "name or service is not known";
    case EAI_AGAIN:
      return "temporary failure in name resolution";
    case EAI_FAIL:
      return "non-recoverable failure in name resolution";
    case EAI_NODATA:
      return "no address associated with name";
    case EAI_FAMILY:
      return "ai_family not supported";
    case EAI_SOCKTYPE:
      return "ai_socktype not supported";
    case EAI_SERVICE:
      return "service not supported for ai_socktype";
    case EAI_ADDRFAMILY:
      return "address family for name not supported";
    case EAI_MEMORY:
      return "memory allocation failure";
    case EAI_SYSTEM:
      return "system error";
    default:
      sprintf(buffer,"gai_error_%02x", errnum);
      return buffer;
  }
}

#endif
