
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


Design 
---------

aprsc's basic design was drawn out in a pizza session in early 2008.  The
design goals were:

* High throughput and small enough latency
* Support for thousands of clients per server
* Support for heavy bursts of new clients (CWOP hits every 5 or 10 minutes)
* Scalability over multiple CPUs
* Low context switch overhead
* Low lock contention between threads

A modern hybrid threaded / event-driven approach was selected.  All recently
developed high-performance Internet servers work in this mode (some with
multiple event-driven processes, some with event-driven threads).  There is
a small, fixed number of threads, close to the number of CPU cores on the
server, so that multiple CPU cores can be utilized, but the relatively
expensive context switches between a high number of threads will not cause
serious overhead.

When the server is under heavy load, data transfers between threads happen
in blocks of multiple data units, so that contention on mutexes and
read-write locks will not block concurrent execution of the threads. Lock
contention makes many multi-threaded servers effectively single-threaded and
unable to utilize more than a single CPU core.

Main work is done by 1 to N worker threads. In real-world APRS-IS today, 1
worker thread is enough, but if a server was really heavily loaded with
thousands of clients, 1 less than the number of CPU cores would be optimal.

A worker thread's workflow goes like this:

1. Read data from connected clients
2. Do initial APRS-IS packet parsing (SRCCALL>DSTCALL,PATH:DATA)
3. Do Q-construct processing (,qAx in the PATH)
4. Parse APRS formatted information in the DATA to extract enough details
   to support filtering in the outgoing / filtering phase
5. Pass on received packets to the dupecheck thread for duplicate removal
6. Get packets, sorted to unique and duplicate packets, from the dupecheck
   thread
7. Send out packets to clients as instructed by the listening port's
   configuration and the client's filter settings

The Dupecheck thread maintains a cache of packets heard during the past 30
seconds.  There is a dedicated thread for this cache, so that the worker
threads do not need to compete for access to the shared resource. The thread
gets packets from the worker threads, does dupe checking, and puts the
unique and duplicate packets in two global ordered buffer queues. The
workers then walk through those buffers and do filtering to decide which
packets should be sent to which clients.

An Uplink threads initiates connections to upstream servers and reconnects
them as needed.  After a successful connection the socket will be passed on
to a Worker thread which will proceed to exchange traffic with the remote
server for the duration of the connection.

An Accept thread listens on the TCP ports for new incoming connections, does
access list checks, and distributes allowed connections evenly across worker
threads.

An HTTP thread runs an event-driven HTTP server (libevent2 based) to support
the status page and HTTP position uploads.  Since implementing nice web user
interfaces in plain C is not very convenient or effective, the status page
is produced using modern Web 2.0 methods.  The HTTP server can only generate
a dynamic JSON-encoded status file and serve static files.  An empty
index.html file loads a static JavaScript file, which then periodically
loads the JSON status data and formats the contents of the status page
within the client's browser.  This approach allowed clean separation of
server code (C) and web presentation (HTML5/JavaScript/jQuery/flot).

Both developers are experienced professional Unix C programmers, so the
programming language was easy to select.  We also had plenty of existing
code that could be re-used in this project.


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

