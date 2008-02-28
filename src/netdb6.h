/*
	IPv6 API additions for the ZMailer at those machines
	without proper libraries and includes.
	By Matti Aarnio <mea@nic.funet.fi> 1997,2004
 */

#ifndef __
# ifdef __STDC__
#  define __(x) x
# else
#  define __(x) ()
# endif
#endif

#ifndef HAVE_GETADDRINFO
#ifndef AI_PASSIVE

struct addrinfo {
  int    ai_flags;	/* AI_PASSIVE | AI_CANONNAME */
  int    ai_family;	/* PF_xxx */
  int    ai_socktype;	/* SOCK_xxx */
  int    ai_protocol;	/* 0, or IPPROTO_xxx for IPv4 and IPv6 */
  size_t ai_addrlen;	/* Length of ai_addr */
  char  *ai_canonname;	/* canonical name for hostname */
  struct sockaddr *ai_addr; /* binary address */
  struct addrinfo *ai_next; /* next structure in linked list */
};


extern int getaddrinfo __(( const char *node, const char *service,
			    const struct addrinfo *hints,
			    struct addrinfo **res ));
extern void freeaddrinfo __(( struct addrinfo *res ));
extern const char *gai_strerror __((int errcode));

#define AI_PASSIVE     1       /* Socket address is intended for `bind'.  */
#endif
#ifndef AI_CANONNAME
#define AI_CANONNAME   2       /* Request for canonical name.  */
#endif
#ifndef AI_NUMERICHOST
#define AI_NUMERICHOST 4       /* Don't use name resolution.  */
#endif

#ifndef EAI_ADDRFAMILY
/* Error values for `getaddrinfo' function.  */
#define EAI_BADFLAGS   -1      /* Invalid value for `ai_flags' field.  */
#define EAI_NONAME     -2      /* NAME or SERVICE is unknown.  */
#define EAI_AGAIN      -3      /* Temporary failure in name resolution.  */
#define EAI_FAIL       -4      /* Non-recoverable failure in name res.  */
#define EAI_NODATA     -5      /* No address associated with NAME.  */
#define EAI_FAMILY     -6      /* `ai_family' not supported.  */
#define EAI_SOCKTYPE   -7      /* `ai_socktype' not supported.  */
#define EAI_SERVICE    -8      /* SERVICE not supported for `ai_socktype'.  */
#define EAI_ADDRFAMILY -9      /* Address family for NAME not supported.  */
#define EAI_MEMORY     -10     /* Memory allocation failure.  */
#define EAI_SYSTEM     -11     /* System error returned in `errno'.  */
#endif

#ifndef NI_MAXHOST
#define NI_MAXHOST	1025
#define NI_MAXSERV	  32

#define NI_NUMERICHOST	0x01
#define NI_NUMERICSERV	0x02
#define NI_NAMEREQD	0x04
#define NI_NOFQDN	0x08
#define NI_DGRAM	0x10
#endif
#endif /* ndef HAVE_GETADDRINFO */
