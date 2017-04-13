
#ifndef RANDOM_H
#define RANDOM_H

extern int urandom_open(void);
extern int urandom_alphanumeric(int fd, unsigned char *buf, int buflen);

#endif

