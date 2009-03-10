/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */
/*
 * Generalized adaptation to ZMailer libc fill-in use by
 * Matti Aarnio <mea@nic.funet.fi> 2000
 *
 * The original version taken from  glibc-2.1.92 on 1-Aug-2000
 *
 * This is SERIOUSLY LOBOTIMIZED to be usable in environments WITHOUT
 * threaded versions of getservbyname(), and friends, plus ridding
 * __alloca()  calls as they are VERY GCC specific, which isn't a good
 * thing for ZMailer.  (Also DE-ANSIfied to K&R style..)
 *
 * Original reason for having   getaddrinfo()  API in ZMailer was
 * to support IPv6 universe -- and that is still the reason.  This
 * adaptation module is primarily for those systems which don't have
 * this IPv6 API, but there are also some systems (all those using
 * the original INNER NET code -- glibc 2.0/2.1/2.2(?) especially)
 * which have faulty error condition processing in them. Specifically
 * plain simple TIMEOUTS on queries are not handled properly!
 *
 * Now that Linuxes have caught up at libc level, we no longer have
 * a reason to support kernel things which don't exist at libc level.
 * (Running ZMailer on Linux with libc5 is not supported in sense of
 * supporting IPv6 at the kernel..)
 *
 *
 *  THIS   getnameinfo()  FUNCTION IS NOT USED IN ZMAILER, BUT IS
 *  SUPPLIED JUST TO COMPLETE THE API IN CASE IT WILL SOMETIME BECOME
 *  USED...
 * 
 */

/* The Inner Net License, Version 2.00

  The author(s) grant permission for redistribution and use in source and
binary forms, with or without modification, of the software and documentation
provided that the following conditions are met:

0. If you receive a version of the software that is specifically labelled
   as not being for redistribution (check the version message and/or README),
   you are not permitted to redistribute that version of the software in any
   way or form.
1. All terms of the all other applicable copyrights and licenses must be
   followed.
2. Redistributions of source code must retain the authors' copyright
   notice(s), this list of conditions, and the following disclaimer.
3. Redistributions in binary form must reproduce the authors' copyright
   notice(s), this list of conditions, and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
4. All advertising materials mentioning features or use of this software
   must display the following acknowledgement with the name(s) of the
   authors as specified in the copyright notice(s) substituted where
   indicated:

	This product includes software developed by <name(s)>, The Inner
	Net, and other contributors.

5. Neither the name(s) of the author(s) nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY ITS AUTHORS AND CONTRIBUTORS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  If these license terms cause you a real problem, contact the author.  */

/* This software is Copyright 1996 by Craig Metz, All Rights Reserved.  */

# include "ac-defs.h"  /* autoconfig environment */

#include <sys/param.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h> /* Sol 2.6 barfs without this.. */
#include <resolv.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#include <sys/utsname.h>
#include <netdb.h>
#if !defined(EAI_AGAIN) || !defined(AI_NUMERICHOST)
# include "netdb6.h"
#endif


#ifndef min
# define min(x,y) (((x) > (y)) ? (y) : (x))
#endif /* min */

#ifndef AF_LOCAL
# ifdef AF_UNIX
#  define AF_LOCAL AF_UNIX
#  define PF_LOCAL PF_UNIX
# endif
#endif

static char * nrl_domainname __((void));
static char *
nrl_domainname ()
{
  static char *domain;
  static int not_first;
  char hostnamebuf[MAXHOSTNAMELEN];

  if (! not_first) {
    char *c;
    struct hostent *h;

    not_first = 1;

    h = gethostbyname ("localhost");

    if (h && (c = strchr (h->h_name, '.')))
      domain = strdup (++c);
    else {
      /* The name contains no domain information.  Use the name
	 now to get more information.  */
      gethostname (hostnamebuf, MAXHOSTNAMELEN);
	
      c = strchr (hostnamebuf, '.');
      if (c)
	domain = strdup (++c);
      else {
	h = gethostbyname(hostnamebuf);

	if (h && (c = strchr(h->h_name, '.')))
	  domain = strdup (++c);
	else {
	  struct in_addr in_addr;

	  in_addr.s_addr = htonl (0x7f000001);

	      
	  h = gethostbyaddr((const char *) &in_addr,
			    sizeof (struct in_addr),
			    AF_INET);

	  if (h && (c = strchr (h->h_name, '.')))
	    domain = strdup (++c);
	}
      }
    }
  }

  return domain;
}

/* This is NASTY, GLIBC has changed the type after instroducing
   this function, Sol (2.)8 has 'int', of upcoming POSIX standard
   revision I don't know.. */

#ifndef GETNAMEINFOFLAGTYPE
# if defined(__GLIBC__) && defined(__GLIBC_MINOR__)
#  if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 2
	/* I am not sure that it was already 2.2(.0) that had
	   this change, but 2.2.2 has it... */
#   define GETNAMEINFOFLAGTYPE unsigned int
#  else
#   define GETNAMEINFOFLAGTYPE int
#  endif
# else
#  define GETNAMEINFOFLAGTYPE int
# endif
#endif

#if 0

int
getnameinfo __((const struct sockaddr *sa, size_t addrlen, char *host,
		size_t hostlen, char *serv, size_t servlen,
		GETNAMEINFOFLAGTYPE flags));

int
getnameinfo (sa, addrlen, host, hostlen, serv, servlen, flags)
     const struct sockaddr *sa;
     size_t addrlen;
     char *host;
     size_t hostlen;
     char *serv;
     size_t servlen;
     GETNAMEINFOFLAGTYPE flags;
{
  int serrno = errno;
  int ok = 0;

  if (sa == NULL || addrlen < sizeof (sa->sa_family))
    return -1;

  switch (sa->sa_family) {
#ifdef AF_LOCAL
  case AF_LOCAL:
    if (addrlen < (size_t) (((struct sockaddr_un *) NULL)->sun_path))
      return -1;
    break;
#endif
  case AF_INET:
    if (addrlen < sizeof (struct sockaddr_in))
      return -1;
    break;
  default:
    return -1;
  }

  if (host != NULL && hostlen > 0)
    switch (sa->sa_family) {
    case AF_INET:
      if (!(flags & NI_NUMERICHOST)) {
	struct hostent *h;

	h = gethostbyaddr((void *) &(((struct sockaddr_in *)sa)->sin_addr),
			  sizeof(struct in_addr), AF_INET);
	if (h) {
	  if (flags & NI_NOFQDN) {
	    char *c;
	    if ((c = nrl_domainname ()) && (c = strstr(h->h_name, c))
		&& (c != h->h_name) && (*(--c) == '.'))     {
	      strncpy (host, h->h_name,
		       min(hostlen, (size_t) (c - h->h_name)));
	      host[min(hostlen - 1, (size_t) (c - h->h_name))] 	= '\0';
	      ok = 1;
	    } else {
	      strncpy (host, h->h_name, hostlen);
	      ok = 1;
	    }
	  } else {
	    strncpy (host, h->h_name, hostlen);
	    ok = 1;
	  }
	}
      }

      if (!ok) {
	if (flags & NI_NAMEREQD) {
	  return -1;
	} else {
	  const char *c;
	  c = inet_ntop (AF_INET,
			 (void *) &(((struct sockaddr_in *) sa)->sin_addr),
			 host, hostlen);
	  if (!c) {
	    return -1;
	  }
	}
	ok = 1;
      }
      break;

#ifdef AF_LOCAL
    case AF_LOCAL:
      if (!(flags & NI_NUMERICHOST)) {
	struct utsname utsname;

	if (!uname (&utsname)) {
	  strncpy (host, utsname.nodename, hostlen);
	  break;
	}
      }

      if (flags & NI_NAMEREQD) {
	return -1;
      }

      strncpy (host, "localhost", hostlen);
      break;
#endif

    default:
      return -1;
    }

  if (serv && (servlen > 0))
    switch (sa->sa_family) {
      case AF_INET:
	if (!(flags & NI_NUMERICSERV)) {
	  struct servent *s;
	  s = getservbyport(((struct sockaddr_in *) sa)->sin_port,
			    ((flags & NI_DGRAM) ? "udp" : "tcp"));
	  if (s) {
	    strncpy (serv, s->s_name, servlen);
	    break;
	  }
	}
	{
	  char decbuf[30];
	  sprintf(decbuf, "%d", ntohs (((struct sockaddr_in *) sa)->sin_port));
	  strncpy(serv, decbuf, servlen);
	  serv[servlen-1] = 0;
	}
	break;

#ifdef AF_LOCAL
      case AF_LOCAL:
	strncpy (serv, ((struct sockaddr_un *) sa)->sun_path, servlen);
	break;
#endif
    }

  if (host && (hostlen > 0))
    host[hostlen-1] = 0;
  if (serv && (servlen > 0))
    serv[servlen-1] = 0;
  errno = serrno;
  return 0;
}
#endif
