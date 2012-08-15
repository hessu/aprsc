
Compiling aprsc from source
===========================

WARNING: THIS DOCUMENT CURRENTLY DOCUMENTS FUTURE FUNCTIONALITY - THE
CONFIGURE SCRIPT IS NOT QUITE FINISHED YET AND YOU CAN'T MAKE INSTALL
EITHER.

If you're familiar with compiling software from the source code, and
pre-built binary packages are not available for your platform, this is where
you need to start.  Binary packages will be provided shortly for Debian and
Ubuntu systems.

aprsc has been built and tested on:

* Ubuntu LTS (10.04, 12.04)
* Debian stable (6.0, "squeeze")
* Debian 5.0 "lenny": i386 and x86_64
* Mac OS X 10.8 (Snow Leopard): x86
* FreeBSD 8.2 and 9.0 on amd64, 7.2 on i386
* Solaris 11 (SunOS 5.11 11.0 i86pc i386)

If you wish to have decent support, please pick Debian or Ubuntu. The other
platforms do work, but when it comes to building and installing, you're
mostly on your own.


Prerequirements and dependencies
-----------------------------------

aprsc requires a recent gcc version (4.1) to compile due to the need for
built-in functions for atomic memory access.

aprsc also requires libevent2 (libevent version 2.0), since libevent's HTTP
server is used to implement the status page and HTTP position upload
services.

libevent2 is available in the latest Linux distributions (apt-get install
libevent or libevent2, but check if it's a 2.0).  Older versions do not come
with it, so you need to download it and compile it from source
(http://libevent.org/).

libevent2 is also available in MacPorts for OS X and FreeBSD ports.

Downloading aprsc
--------------------

Head to http://he.fi/aprsc/down/

Compiling aprsc
------------------

    Delete old version if necessary:
    $ rm aprsc-latest.tar.gz
    
    Download the latest source tree:
    $ wget http://he.fi/aprsc/down/aprsc-latest.tar.gz
    
    Extract it:
    $ tar xvfz aprsc-latest.tar.gz

    Go to the newly-created directory and configure the build:
    $ cd aprsc-1.0.0
    $ ./configure
    
At this point the configuration either succeeds or fails. If it fails, it is
probably due to a missing dependency, in which case it tries to tell clearly
what is missing.  If your platform does not appear in the list of tested
platforms, the failures might be more "interesting".

    Compile it:
    $ make

    Install it:
    $ make install
    
    Install example configuration:
    $ make installconf





