
Notes to iGate developers
==============================

This document tries to document some common bugs which have in the past
popped up in multiple different APRS applications.  Hopefully developers of
new iGates and other APRS apps will see this and avoid a few pitfalls which
have caused us problems before.


Packets modified by iGates
-----------------------------

If an iGate modifies APRS packet data content, for example, by removing
spaces from the end of the line, or by removing or replacing non-ASCII
bytes, before it gets forwarded to the APRS-IS (or otherwice retransmitted),
it is creating a *modified duplicate* of the packet.

Some popular iGates have even removed non-ascii binary bytes very commonly
transmitted by all Mic-E trackers, or replaced them with spaces.

The same packet will likely heard by other iGates, and those other iGates
will not do the same modifications that you do, and then there will be two
slightly different copies of the packet on the APRS network.

***Solution:***

Please do not modify packet data.  Do not trim spaces from the end, do not
remove non-ASCII bytes such as 0x1C or 0x00.  Just send everything on the
first line, up to the newline.


Packets truncated by iGates due to C string handling
-------------------------------------------------------

Several different iGates in the past have used C-style string functions
(strcpy, strlen, strncat...) when processing and copying packets around.
These functions stop reading the source string whenever they see a NUL byte
(byte with value 0x00).

* Sometimes Unicode text is (incorrectly) encoded as UTF-16 instead of UTF-8.
UTF-16 uses NUL bytes a lot.
* Some common APRS transmitters have bugs, causing NUL bytes to be inserted.

If an iGate stops copying the packet at the first NUL byte, it is
truncating the packet and creating a *modified duplicate* of it. Another
iGate does not truncate the packet, and there are now two copies of it, both
getting forwarded over the APRS network.

***Solution:***

Instead of C-style string functions (strcmp, strcpy, strncat...), use binary
safe memory comparison and copying functions (memcmp, memcpy, memchr...).

Many programming languages (Python, Perl, Java, Pascal, and a few others)
which do not use C-style 0-terminated strings do not natively have this
problem.  If you are working in one of these environments, you should be
fine.


Packets getting modified due to character encoding issues
------------------------------------------------------------

While APRS text messages, comment and status strings may contain UTF-8
encoded strings, the UTF-8 encoding only applies to those string fields. 
Many APRS packets contain binary byte sequences which are not correct UTF-8,
and may even contain NUL bytes.

Thus, if an application tries to do UTF-8 decoding on the whole APRS-IS
stream, or received complete APRS packets, that will often fail, and may
sometimes result in a *modified duplicate* of the packet.  If the
application retransmits that modified copy, possibly after re-encoding,
there will be two slightly different packets again, since other applications
will pass the original packet unmodified.

***Solution:***

Treat APRS packets as arrays or buffers of binary bytes.  Only after
decoding the APRS packet, some fields such as the comment text and APRS text
message text should be UTF-8 decoded.

If your programming language or environment supports setting text encoding
for a network socket, do not set UTF-8 encoding for the APRS-IS socket.
Treat it as a binary stream.


iGates not supporting DNS
----------------------------

There are some iGate appliances out there which require the user to put in
an IP address of an APRS-IS server.

The problem is that the APRS-IS servers sometimes come and go, and their IP
addresses change. Many iGates are left running unsupervised for months,
sometimes at remote locations.

***Solution:***

Implement DNS in the iGate.  Instruct the user to configure a round-robin
DNS hostname as the APRS-IS server (such as rotate.aprs2.net), instead of an
IP address or hostname of a single specific server.


iGates only doing DNS lookup once at start-up, and caching the IP address
----------------------------------------------------------------------------

Some iGates only do a DNS lookup for a hostname when they start up, 
and then use the IP address or addresses without doing a DNS lookup again.

* The IP address of a server might well change while the iGate is running.
* Some servers, such as all servers on the aprs2.net network, use dynamic
  DNS load balancing to point clients to the servers which statistically
  work best and have the least users at any given time. You'll get a
  different set of IP addresses for rotate.aprs2.net when asking again
  in 15 minutes.

Caching IP addresses breaks load balancing and might leave the iGate
unconnected if the server at that address does not work any more.

***Solution:***

Do not cache DNS responses. Do a new DNS lookup every time when making a new
TCP connection to an APRS-IS server.


Multiple connections
-----------------------

A few well-meaning authors have also tried to connect to multiple APRS-IS
servers at the same time, sending packets to both of them, and even in some
case sending packets coming from one server to the second server.

Unfortunately the APRS-IS network is not designed to handle this.  TCP
connections often slow down a lot when a few packets are lost (TCP uses
exponential backoff), and packets on one connection may be delayed for quite
a while, without the application noticing.  Since the APRS-IS only detects
duplicates within a 30-second window, having multiple concurrent connections
creates a loop and will, every now and then, cause bursts of duplicate
delayed packets.

These delayed duplicates will then cause cars to jump back to their old
positions on real-time map displays, and all sorts of other havoc.

***Solution:***

Only have a single connection to the APRS-IS active at any time.

For the same reason, do not buffer packets when your APRS-IS uplink
connection is down. Do not send them later when the connection recovers.
Some other iGate has probably already sent them, and your delayed packets
would cause moving vehicles to jump around in odd ways.

