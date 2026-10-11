
#ifndef RANDOM_H
#define RANDOM_H

extern int urandom_open(void);
extern void urandom_init(void);
extern int urandom_alphanumeric(int fd, unsigned char *buf, int buflen);
extern int urandom_token(unsigned char *buf, int buflen);

#endif

