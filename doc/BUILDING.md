
Compiling aprsc from source
===========================

If you're familiar with compiling software from the source code, and
pre-built binary packages are not available for your platform, this is where
you need to start.  Binary packages will be provided shortly for Debian and
Ubuntu systems.

aprsc has been built and tested on:

* Ubuntu LTS (10.04, 12.04)
* Debian stable (6.0, "squeeze")
* Debian 5.0 ("lenny"): i386 and x86_64
* Mac OS X 10.8 (Snow Leopard): x86
* FreeBSD 9.0 amd64
* Solaris 11 (SunOS 5.11 11.0 i86pc i386)

If you wish to have decent support, please pick Debian or Ubuntu. The other
platforms do work, but when it comes to building and installing, you're
mostly on your own.


Prerequirements
------------------

aprsc requires a recent gcc version (4.1) to compile due to the need for
built-in functions for atomic memory access.

aprsc also requires libevent2 (libevent version 2.0), since libevent's HTTP
server is used to implement the status page and HTTP position upload
services.

libevent2 is available in the latest Linux distributions (apt-get install
libevent or libevent2, but check if it's a 2.0).  Older versions do not come
with it, so you need to download it and compile it from source
(http://libevent.org/).


