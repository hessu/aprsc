all: aprsc

CC = gcc
LD = gcc
CFLAGS = -Wall -Wstrict-prototypes -g -D_REENTRANT
#CFLAGS = -Wall -Wstrict-prototypes -O3 -D_REENTRANT
LDFLAGS = -lpthread 

# Linux:
# -lpthread
# Solaris 2.8:
# -lpthread -lxnet -lsocket -lnss -lrt
# Solaris 2.6:
# -lpthread -lxnet -lsocket -lnsl -lposix4 -lresolv

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o *~ */*~ core
distclean: clean
	rm -f aprsc

BITS = aprsc.o accept.o worker.o \
	login.o incoming.o dupecheck.o outgoing.o \
	config.o netlib.o xpoll.o \
	cfgfile.o passcode.o \
	rwlock.o hmalloc.o hlog.o \
	splay.o spsymbol.o crc32.o

aprsc: $(BITS)
	$(LD) $(LDFLAGS) -o aprsc $(BITS)


aprsc.o:	aprsc.c hmalloc.h hlog.h config.h splay.h accept.h
accept.o:	accept.c accept.h hmalloc.h hlog.h config.h netlib.h worker.h dupecheck.h
worker.o:	worker.c worker.h hmalloc.h hlog.h config.h xpoll.h incoming.h outgoing.h
login.o:	login.c login.h hmalloc.h hlog.h worker.h passcode.h incoming.h config.h
incoming.o:	incoming.c incoming.h hmalloc.h hlog.h worker.h
outgoing.o:	outgoing.c outgoing.h hlog.h worker.h
dupecheck.o:	dupecheck.c dupecheck.h hmalloc.h hlog.h worker.h
passcode.o:	passcode.c passcode.h
xpoll.o:	xpoll.c xpoll.h hmalloc.h hlog.h
netlib.o:	netlib.c netlib.h worker.h
rwlock.o:	rwlock.c rwlock.h
hmalloc.o:	hmalloc.c hmalloc.h
hlog.o:		hlog.c hlog.h hmalloc.h rwlock.h
cfgfile.o:	cfgfile.c cfgfile.c hmalloc.h
config.o:	config.c config.h cfgfile.h hmalloc.h hlog.h
splay.o:	splay.c splay.h hmalloc.h config.h
spsymbol.o:	spsymbol.c splay.h hmalloc.h crc32.h
crc32.o:	crc32.c crc32.h
netlib.o:	netlib.c netlib.h
worker.h:	rwlock.h xpoll.h
