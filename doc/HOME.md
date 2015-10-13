
aprsc - an APRS-IS server in C
==============================

aprsc (pronounced a-purrs-c) is a plain APRS-IS server intended to be used
on the core and Tier2 APRS-IS servers.  It is written in the C language, and
it runs on Linux and Unix servers.

If you need igate or other radio-interfacing features, aprsc is not for you.


Status of the project
------------------------

aprsc was released in 2012, and it has since been in continuous use on a
large percentage of APRS-IS servers.  It has been found to be easy to set up
and stable in production use.  Since it is pretty much feature complete by
now, new major versions come out rarely, but smaller bugs get fixed at
times.


Features (and lack of)
-------------------------

aprsc has been designed strictly for use within the APRS-IS core, hub and
Tier2 servers.  It includes only the basic functionality required by those
servers.

It does not, and will not, have any additional functions such as igating,
digipeating, interfacing to radios, D-PRS or other gateway functions, or
object generation.  The idea is to keep aprsc relatively simple and lean,
and leave the more specialized features for more specialized software.

If you need a nice, compact igate software for Linux, please take a look at
either aprx, or aprs4r.  If you need to run an APRS-IS server on some
platform not supported by aprsc, or if you need the features existing in
javAPRSSrvr which are missing from aprsc, javAPRSSrvr is the right choice
for you - it's got a lot of good features that many of you need, and it
works on virtually all operating systems.  If you need an igate for Windows,
APRSIS32 should be good.


Licensing, environments and requirements
-------------------------------------------

aprsc is open source, licensed under the BSD license. It has about 11000
lines of relatively clean C code, built using the usual ./configure && make
&& make install method.  The embedded HTTP status server is powered by the
libevent2 library, no other extra libraries are needed.

Linux and OS X are the main development environments and will receive
premium support, but FreeBSD and Solaris 11 are known to work too.  Packaged
binaries for Debian, Ubuntu and CentOS are available for super-easy
installations and automatic upgrades using APT and YUM.


Discussion group
-------------------

aprsc has it's own [discussion group][aprsc-group] which also functions as
a mailing list. If you run aprsc, please subscribe to the group to keep
updated on new versions.

[aprsc-group]: https://groups.google.com/forum/#!forum/aprsc


Getting and installing aprsc
-------------------------------

aprsc is currently best supported on Debian and Ubuntu.
Please refer to the [INSTALLING](INSTALLING.html) document
for instructions.

After the software is installed, please go through the
[CONFIGURATION](CONFIGURATION.html) document.


Other documentation
----------------------

* [README](README.html)
* [Paper on aprsc for TAPR DCC 2012](dcc-2012-aprsc.pdf)
* Presentation slides from TAPR DCC 2012 (will be here shortly)


Contributing to aprsc
------------------------

aprsc is an open source project, so you're welcome to contribute bug fixes
and improvements.  Please see [CONTRIBUTING](CONTRIBUTING.html) for details!


