
#ifndef ACL_H
#define ACL_H

#include <netinet/in.h>

struct acl_e4_t {
	uint32_t addr;
	uint32_t mask;
	int allow;
	struct acl_e4_t *next;
};

struct acl_e6_t {
	uint32_t addr[4];
	uint32_t mask[4];
	int prefixlen;
	int allow;
	struct acl_e6_t *next;
};

struct acl_t {
	struct acl_e4_t *entries4;
	struct acl_e6_t *entries6;
};

extern struct acl_t *acl_new(void);
extern void acl_free(struct acl_t *acl);
extern struct acl_t *acl_dup(struct acl_t *acl);

extern int acl_add(struct acl_t *acl, char *netspec, int allow);

extern struct acl_t *acl_load(char *s);

extern int acl_check(struct acl_t *acl, struct sockaddr *sa, int addr_len);

#endif
