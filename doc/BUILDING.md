
Compiling aprsc from source
===========================

If you're familiar with compiling software from the source code, and
pre-built binary packages are not available for your platform, this is where
you need to start.  Binary packages are provided for Debian and Ubuntu
systems.

aprsc has been built and tested on:

* Ubuntu LTS (12.04, 14.04, 16.04): i386 and x86_64
* Debian 7.0, "wheezy": i386 and x86_64
* Debian 8.0, "jessie": i386 and x86_64
* Mac OS X 10.8 (Snow Leopard): x86
* FreeBSD 8.2 and 9.0 on amd64, 7.2 on i386
* Solaris 11 (SunOS 5.11 11.0 i86pc i386)
* Raspberry Pi (debian 7, Raspbian): ARM11/ARMv6
* Windows 8 (Cygwin)

If you wish to have decent support, please pick Ubuntu or Debian. The other
platforms do work, but when it comes to building and installing, you're
mostly on your own.


Prerequirements and dependencies
-----------------------------------

aprsc requires a recent gcc version (4.1 or later) to compile due to the
need for built-in functions for atomic memory access.

aprsc also requires libevent2 (libevent version 2.0), since libevent's HTTP
server is used to implement the status page and HTTP position upload
services.

libevent2 is available in the most recent Linux distributions (apt-get install
libevent or libevent2, but check if it's a 2.0).  Older versions do not come
with it, so you need to download it and compile it from source
(http://libevent.org/).

libevent2 is also available in MacPorts for OS X and FreeBSD ports.

On Linux, aprsc can utilize POSIX capabilities to enable binding low
(< 1024) ports while not running as root. This requires the libcap
library, and compiling against it requires it's development headers
(package libcap-dev on Debian/Ubuntu).


Building a .deb package
--------------------------

If you're building for a Debian or Ubuntu system, it's generally easiest to
build a debian package and install it.  It'll make installation easy, since
you'll get the package scripts to do the environment setup and upgrades for
you.

Download the latest source tree:

    $ wget http://he.fi/aprsc/down/aprsc-latest.tar.gz
    
Extract it:

    $ tar xvfz aprsc-latest.tar.gz
    
Go to the newly-created directory and configure the build:

    $ cd aprsc-1.0.0 (or whatever)
    $ cd src
    $ ./configure
    $ make make-deb

As a result you'll get a nice .deb package, which you can install with:

    $ sudo dpkg -i ../aprsc-something.deb


Preparing the environment
----------------------------

Create user account and group for aprsc. aprsc should be started as root,
but a non-privileged user account must be provided, so that it can switch to
that after starting up. Doing this is not required if you're installing a
.deb or .rpm package - the package scripts take care of the setup.

Linux (single command, all parameters are for adduser):

    adduser --system --no-create-home --home /var/run/aprsc
        --shell /usr/sbin/nologin --group aprsc


Compiling aprsc
------------------

Delete old version if necessary:

    $ rm aprsc-latest.tar.gz
    
Download the latest source tree:

    $ wget http://he.fi/aprsc/down/aprsc-latest.tar.gz
    
Extract it:

    $ tar xvfz aprsc-latest.tar.gz
    
Go to the newly-created directory and configure the build:

    $ cd aprsc-1.0.0 (or whatever)
    $ cd src
    $ ./configure

At this point the configuration either succeeds or fails. If it fails, it is
probably due to a missing dependency, in which case it tries to tell clearly
what is missing.  If your platform does not appear in the list of tested
platforms, the failures might be more "interesting".

On FreeBSD, with libevent2 installed from ports, you'll have to do this:

    CFLAGS=-I/usr/local/include LDFLAGS=-L/usr/local/lib/event2 ./configure

On Mac OS X, with libevent2 installed from MacPorts, use:

    CFLAGS=-I/opt/local/include LDFLAGS=-L/opt/local/lib ./configure

If you've installed libevent2 from sources with it's default configuration
on any Unix-like system, the FreeBSD example above (pointing to /usr/local)
should probably work.

With build configuration done:

Compile it:

    $ make
    
Install it:

    $ sudo make install
    
You should now have a nice installed set of software in /opt/aprsc.
An example configuration file has been installed as
/opt/aprsc/etc/aprsc.conf. An existing configuration file will not be
overwritten by a subsequent install.


A note on the chroot
-----------------------

aprsc is usually run in a  chroot, which prevents it from accessing any
files outside of the chroot directory after it has started up.  If the
default path of internal logging to file and internal log rotation is used,
the logs need to be below the chroot directory.  Since configuration reloads
need to be supported, the configuration needs to be there too.  This is why
aprsc is installed within a single /opt/aprsc directory tree with configs in
/opt/aprsc/etc and logs in /opt/aprsc/logs.

It is possible to enable logging to syslog by reconfiguring syslogd to
provide an additional UNIX domain socket within the chroot.  However, due to
the various different syslogds in wide use, the default aprsc installations
do not make any attempt at this.


Downloading older versions of aprsc
--------------------------------------

Head to [source code downloads](http://he.fi/aprsc/down/) !

It's also possible to browse and download source code and inspect changes
made in the software in [github](https://github.com/hessu/aprsc), which is
used for version control and patch management in aprsc.

