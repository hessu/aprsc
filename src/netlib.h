#ifndef NETLIB_H
#define NETLIB_H

extern int aptoa(struct in_addr sin_addr, int sin_port, char *s, int len);
extern int h_strerror(int i, char *s, int len);

#endif
