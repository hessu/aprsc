
/*
 *	ACL matching
 *
 *	(c) Tomi Manninen <tomi.manninen@hut.fi>
 *	(c) Heikki Hannikainen
 *
 */

#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hmalloc.h"
#include "acl.h"
#include "worker.h"
#include "hlog.h"
#include "cfgfile.h"

/*
 *	allocate an empty acl structure
 */

struct acl_t *acl_new(void)
{
	struct acl_t *a;
	
	a = hmalloc(sizeof(struct acl_t));
	
	a->entries4 = NULL;
	a->entries6 = NULL;
	
	return a;
}

void acl_free(struct acl_t *acl)
{
	struct acl_e4_t *e4;
	struct acl_e6_t *e6;
	
	while (acl->entries4) {
		e4 = acl->entries4->next;
		hfree(acl->entries4);
		acl->entries4 = e4;
	}
	
	while (acl->entries6) {
		e6 = acl->entries6->next;
		hfree(acl->entries6);
		acl->entries6 = e6;
	}
	
	hfree(acl);
}

struct acl_t *acl_dup(struct acl_t *acl)
{
	struct acl_e4_t *e4, *ne4;
	struct acl_e6_t *e6, *ne6;
	struct acl_t *a = acl_new();
	
	for (e4 = acl->entries4; (e4); e4 = e4->next) {
		ne4 = hmalloc(sizeof(*ne4));
		memcpy(ne4, e4, sizeof(*ne4));
		ne4->next = a->entries4;
		a->entries4 = ne4;
	}
	
	for (e6 = acl->entries6; (e6); e6 = e6->next) {
		ne6 = hmalloc(sizeof(*ne6));
		memcpy(ne6, e6, sizeof(*ne6));
		ne6->next = a->entries6;
		a->entries6 = ne6;
	}
	
	return a;
}

/*
 *	Add an entry to the ACL
 */

int acl_add(struct acl_t *acl, char *netspec, int allow)
{
	int prefixlen = 128;
	char *prefixls;
	struct addrinfo req, *ai, *nextai;
	int i;
	char dummyservice[] = "12345";
	struct acl_e4_t *e4;
	struct acl_e6_t *e6;
	char *addr_s;
	union sockaddr_u *sup;

	prefixls = strchr(netspec, '/');
	if (prefixls) {
		// has netmask or prefix length
		*prefixls = 0;
		prefixls++;
	}
	
	// prepare for getaddrinfo
	memset(&req, 0, sizeof(req));
	req.ai_family   = 0;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;
	req.ai_flags    = 0;
	ai = NULL;
	
	// resolve the name
	i = getaddrinfo(netspec, dummyservice, &req, &ai);
	if (i != 0) {
		hlog(LOG_ERR, "ACL: address parse failure of '%s': %s", netspec, gai_strerror(i));
		return -1;
	}
	
	// parse the prefix length
	if (prefixls) {
		prefixlen = atoi(prefixls);
		if (prefixlen < 0 || prefixlen > 128) {
			hlog(LOG_ERR, "ACL: invalid prefix len '%s' for '%s'", prefixls, netspec);
			return -1;
		}
	}
	
	while (ai) {
		nextai = ai->ai_next;
		addr_s = strsockaddr( ai->ai_addr, ai->ai_addrlen );
		sup = (union sockaddr_u *)ai->ai_addr;
		
		if (ai->ai_family == AF_INET6) {
			//hlog(LOG_DEBUG, "ACL: Adding IPv6: %s/%d: %s/%d", netspec, prefixlen, addr_s, prefixlen);
			
			e6 = hmalloc(sizeof(*e6));
			e6->next = acl->entries6;
			acl->entries6 = e6;
			e6->allow = allow;
			
			memcpy(e6->addr, sup->si6.sin6_addr.s6_addr, sizeof(e6->addr));
			e6->prefixlen = prefixlen;
			if (prefixlen == 128) {
				memset(e6->mask, 0xff, 16);
			} else {
				memset(e6->mask, 0, 16);
				uint32_t shift = prefixlen;
				uint32_t s;
				
				for (i = 0; i < 4; i++) {
					s = (shift > 32) ? 32 : shift;
					shift -= s;
					
					e6->mask[i] = 0UL - (1UL << (32UL - s));
					/* mask the address wih the mask so that no host bits are set */
					e6->addr[i] = e6->addr[i] & e6->mask[i];
				}
			}
			
		} else if (ai->ai_family == AF_INET) {
			if (prefixlen > 32)
				prefixlen = 32;
			
			//hlog(LOG_DEBUG, "ACL: Adding IPv4: %s/%d", netspec, prefixlen);
			
			e4 = hmalloc(sizeof(*e4));
			e4->next = acl->entries4;
			acl->entries4 = e4;
			e4->allow = allow;
			
			e4->addr = ntohl(sup->si.sin_addr.s_addr);
			e4->mask = 0;
			for (i = 0; i < prefixlen; i++) {
				e4->mask = e4->mask >> 1;
				e4->mask |= 1<<31;
			}
			e4->addr = e4->addr & e4->mask;	
		}
		
		hfree(addr_s);
		hfree(ai);
		ai = nextai;
	}
	
	return 0;
}

#define ACL_LINEBUF 1024

struct acl_t *acl_load(char *s)
{
	struct acl_t *acl;
	FILE *fp;
	char buf[ACL_LINEBUF];
	char *eol;
	char *argv[256];
	int argc;
	int failed = 0;
	int line = 0;
	
	//hlog(LOG_DEBUG, "ACL: Loading ACL file \"%s\"", s);
	
	fp = fopen(s, "r");
	if (!fp) {
		hlog(LOG_ERR, "ACL load failed: Could not open \"%s\" for reading: %s", s, strerror(errno));
		return NULL;
	}
	
	acl = acl_new();
	
	while (fgets(buf, ACL_LINEBUF, fp)) {
		line++;
		// strip newlines
		eol = strchr(buf, '\r');
		if (eol)
			*eol = 0;
		eol = strchr(buf, '\n');
		if (eol)
			*eol = 0;
			
		// strip comments
		eol = strchr(buf, '#');
		if (eol)
			*eol = 0;
		
		// skip empty lines
		if (strlen(buf) == 0)
			continue;
		
		argc = parse_args(argv, buf);
		if (argc != 2) {
			hlog(LOG_ERR, "ACL load failed: %s: invalid number of arguments on line %d", s, line);
			failed = 1;
		}
		
		int allow = 0;
		if (strcasecmp(argv[0], "allow") == 0)
			allow = 1;
		else if (strcasecmp(argv[0], "deny") == 0)
			allow = 0;
		else {
			hlog(LOG_ERR, "ACL load failed: %s: invalid command on line %d: %s", s, line, argv[0]);
			failed = 1;
			continue;
		}
		
		if (acl_add(acl, argv[1], allow)) {
			hlog(LOG_ERR, "ACL load failed: %s: invalid netspec on line %d: %s", s, line, argv[1]);
			failed = 1;
		}
	}
	
	if (fclose(fp)) {
		hlog(LOG_ERR, "ACL load failed: close(%s): %s", s, strerror(errno));
		failed = 1;
	}
	
	if (failed) {
		acl_free(acl);
		return NULL;
	}
	
	return acl;
}

/*
 *	Match an IPv4 address agaist the ACL list
 */
 
static int acl_check_4(struct acl_t *acl, uint32_t addr)
{
	struct acl_e4_t *a = acl->entries4;
	
	while (a) {
		if ((addr & a->mask) == a->addr)
			return a->allow;
		a = a->next;
	}
	
	return 0;
}

/*
 *	Match an IPv6 address agaist the ACL list
 */

static int acl_check_6(struct acl_t *acl, uint32_t addr[4])
{
	struct acl_e6_t *a = acl->entries6;
	int i;
	
	//hlog(LOG_DEBUG, "acl_check_6");
	
	while (a) {
		int fail = 0;
		for (i = 0; i < 4 && !fail; i++) {
			if ((addr[i] & a->mask[i]) != a->addr[i]) {
				fail = 1;
			}
		}
		
		if (!fail) {
			return a->allow;
		}
		
		a = a->next;
	}
	
	// Disallow by default
	return 0;
}

/*
 *	Match a sockaddr against the ACL
 */

int acl_check(struct acl_t *acl, struct sockaddr *sa, int addr_len)
{
	union sockaddr_u su, *sup;
	sup = (union sockaddr_u *)sa;
	
	/* When we're listening on [::] address, IPv4 connections will be
	 * accepted too. The IPv4 source address will be given to mapped
	 * to an IPv6 address. Here, we map that back to an IPv4 address.
	 */
#ifdef IN6_IS_ADDR_V4MAPPED
	if ( sa->sa_family == AF_INET6 && 
	     ( IN6_IS_ADDR_V4MAPPED(&(sup->si6.sin6_addr)) ||
	       IN6_IS_ADDR_V4COMPAT(&(sup->si6.sin6_addr)) ) ) {

		memset(&su, 0, sizeof(su));
		su.si.sin_family = AF_INET;
		su.si.sin_port   = sup->si6.sin6_port;
		memcpy(& su.si.sin_addr, &((uint32_t*)(&(sup->si6.sin6_addr)))[3], 4);
		sa = &su.sa;
		sup = (union sockaddr_u *)sa;
		// hlog(LOG_DEBUG, "Translating v4 mapped/compat address..");
	}
#endif
	
	
	if (sup->sa.sa_family == AF_INET)
		return acl_check_4(acl, ntohl(sup->si.sin_addr.s_addr));
	if (sup->sa.sa_family == AF_INET6)
		return acl_check_6(acl, (uint32_t *)sup->si6.sin6_addr.s6_addr);
	
	hlog(LOG_ERR, "acl_check failed: unknown address family or address length (family %d, length %d)", sup->sa.sa_family, addr_len);
	
	return 0; // Disallow by default: unknown address family
}

