
#ifndef CRC32_H
#define CRC32_H

extern void crcinit(void);
extern unsigned long crc32n(const unsigned char *s, int slen);
extern unsigned long crc32(const unsigned char *s);

#endif
