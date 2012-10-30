
aprsc - an APRS-IS server in C
==============================

aprsc (pronounced a-purrs-c) is a plain APRS-IS server intended to be used
on the core and Tier2 APRS-IS servers.  It is written in the C language, and
it runs on Linux and Unix servers.

If you need igate or other radio-interfacing features, aprsc is not for you.

If you need to run a server on Windows, aprsc is not for you. Sorry!


A word of caution
--------------------

The aprsc software is brand new, under active development, and release
cycles are currently very quick - be ready to upgrade on a short notice when
new versions are announced.  Like any new software, aprsc is likely to
contain new bugs, and some upgrades might have a high priority.

If you're not prepared to upgrade often, please wait for a few months, it
should be more peaceful then.  Documentation is not there yet, either, but
if you're an experienced APRS-IS server operator, there should be no big
surprises around.


Features (and lack of)
-------------------------

aprsc has been designed strictly for use within the APRS-IS core, hub and
Tier2 servers.  It includes only the basic functionality required by those
servers:

* Duplicate packet filtering
* Q construct processing
* Client-defined filters
* APRS packet parsing as necessary to support filtering
* i-gate client support
* Messaging support
* UDP client support
* UDP core peer links
* Uplink server support
* Passcode validation
* Web status page + Machine-readable JSON status on an HTTP server
* HTTP position upload using POST
* Full IPv4 and IPv6 support
* Configurable access lists on client ports
* Logging to either syslog, file or stderr
* Built-in log rotation when logging to a file
* Runs in a chroot

A little additional feature sugar has been added on top:

* Online reconfiguration of almost all settings without restarting
* Live upgrade - software can usually be upgraded without disconnecting
  clients
* Munin plugin for statistics graphs

It does not, and will not, have any additional functions such as igating,
digipeating, interfacing to radios, D-PRS or other gateway functions, or
object generation.  It will not work on Windows.  The idea is to keep aprsc
relatively simple and lean, and leave the more specialized features for more
specialized software.

If you need a nice, compact igate software for Linux, please take a look at
either aprsg, aprx, or aprs4r.  If you need to run an APRS-IS server on
Windows or some other platform not supported by aprsc, or if you need the
features existing in javAPRSSrvr which are missing from aprsc, javAPRSSrvr
is the right choice for you - it's got a lot of good features that many of
you need, and it works on virtually all operating systems.  If you need an
igate for Windows, APRSIS32 should be good.


Licensing, environments and requirements
-------------------------------------------

aprsc is open source, licensed under the BSD license. It has about 11000
lines of relatively clean C code, built using the usual ./configure && make
&& make install method.  The embedded HTTP status server is powered by the
libevent2 library, no other extra libraries are needed.

Linux and OS X are the main development environments and will receive
premium support, but FreeBSD and Solaris 11 are known to work too.  Packaged
binaries for Debian and Ubuntu are available for super-easy installations
and automatic upgrades using APT.


Performance
--------------

TODO: figures here


Quality control
------------------

aprsc comes with an APRS-IS server test suite, implemented using the Perl
Test framework.  A "make test" executed in the tests/ subdirectory will
execute automated tests for all of the basic functions of the server in
about 2 minutes.  Individual test scripts run fake APRS-IS servers, clients
and peers around the tested server, and pass various valid and invalid
packets through the server, checking for expected output.

Test-driven development methods have been used during the development: a
testing script has been implemented first, based on existing documentation
and wisdom learned from the mailing lists and communication with other
developers, the test case has been validated to match the functionality of
javAPRSSrvr (by running javAPRSSrvr through the test suite), and only then
the actual feature has been implemented in aprsc.  This approach should
ensure a good level of compatibility between the components and prevent old
bugs from creeping back in.


Who's who, and how long did it take
--------------------------------------

aprsc has been developed between 2008 and 2012 by Matti Aarnio, OH2MQK
(aprx, zmailer, etc), and Heikki Hannikainen, OH7LZB (aprs.fi).  Design
phase and most of the core development happened during 2008, but the final
sprint for feature completeness happened during the summer of 2012. 
Substantial code reuse happens between aprx, aprsc and other projects of the
authors.

aprsc was inspired by the performance problems experienced on the core
APRS-IS servers in early 2008, which were mitigated by moving CWOP clients
to their dedicated servers.  It has also been driven by the desire for a
free, open-source server.


Discussion group
-------------------

aprsc has it's own discussion group and mailing list. If you run aprsc,
please subscribe to the list to keep updated on new versions.

https://groups.google.com/forum/#!forum/aprsc

