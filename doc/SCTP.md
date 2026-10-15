
APRS-IS and IS2 over SCTP
----------------------------

aprsc can carry both the plaintext APRS-IS protocol and the binary IS2
protocol over SCTP, in addition to TCP.  This document describes how the
data is put into SCTP messages, which framing is done inside them, and
why.

Work in progress, like IS2 itself.


Where SCTP is used
---------------------

SCTP is available on Linux, when aprsc is built with the SCTP headers
(`netinet/sctp.h`, the libsctp-dev or lksctp-tools-devel package).  It
can be used on:

* Listeners of any client port type: the plaintext types (fullfeed,
  igate, clientonly, dupefeed) and the IS2 types (is2fullfeed, is2igate,
  is2dupefeed):

        Listen "Full feed SCTP"     fullfeed    sctp ::0  10152
        Listen "IS2 Full feed SCTP" is2fullfeed sctp ::0  20152

  A TCP and an SCTP listener can use the same port number.

* Uplinks, plaintext or IS2:

        Uplink core1 full sctp core1.example.com 10152
        Uplink core2 full sctp core2.example.com 20152 is2

The UDP peer links of a `PeerGroup` are not available over SCTP: the
configuration parser accepts `sctp` there, but the peer link code handles
UDP datagrams only.

aprsc uses one-to-one style SCTP sockets (`SOCK_STREAM`, `IPPROTO_SCTP`):
each client or uplink connection is an SCTP association of its own,
accepted and connected like a TCP connection.

An association is multi-homed when either end has more than one address.
aprsc binds a listener to the address given in its configuration, and an
uplink to the `UplinkBind` address if one is configured, otherwise to
none.  An end bound to a wildcard address (`0.0.0.0` or `::0`), or to
none, advertises all the addresses of its host when the association is
set up, and the kernel moves the traffic to another of them if the path
to one fails.  An end bound to a single address has only that one.  The
kernel leaves private and link-local addresses out of an association
with a peer on a global address (`net.sctp.addr_scope_policy`).
Firewalls have to pass SCTP to and from each of the addresses, and
multi-homing does not work through NAT.


Failover
-----------

Each address of the peer is a path of its own, with heartbeats sent on
it to find out whether it works.  aprsc sends them every 15 seconds
instead of the default 30 (`SCTP_PEER_ADDR_PARAMS`), so that the state of
the alternate paths is known before the traffic has to move to one of
them.

The kernel moves the traffic to another working path after the first
retransmission timeout on a path, when the path becomes "potentially
failed" (RFC 7829).  This is the default of Linux (`net.sctp.pf_retrans`
0), and aprsc sets it on its sockets (`SCTP_PEER_ADDR_THLDS`), so that it
does not depend on the host's setting.  The retransmission timeout is at
least a second (`net.sctp.rto_min`), so the packets sent when a path
fails are delayed by about a second, and none are lost.  An address
which has not yet been confirmed with a heartbeat, a few seconds after
the association is set up, can not be used.

The failed path is probed with heartbeats at intervals of the
retransmission timeout, which doubles after each failure up to a minute
(`net.sctp.rto_max`), and the traffic moves back to it when it answers.
There is no hurry to return, since the alternate path works.  If all the
paths fail, an uplink is closed by aprsc after `UpstreamTimeout`, and a
client after 10 retransmissions in a row
(`net.sctp.association_max_retrans`), some five minutes.

aprsc logs a path becoming unreachable, after more than 5 retransmission
timeouts on it (`net.sctp.path_max_retrans`), and becoming available
again.  The move of the traffic after the first timeout is logged only if
the notification of the potentially failed state is enabled with
`net.sctp.pf_expose = 2`.


Messages, not a stream
-------------------------

TCP carries a stream of bytes, and the receiver has to find the packets in
it.  SCTP carries messages: what the sender passes to one `send()` arrives
as one message, and the receiver gets it from one `recvmsg()`.  aprsc
relies on this, with two rules:

1. **A message carries only complete lines or frames.**  An APRS-IS line,
   or an IS2 frame, is never split across two SCTP messages.  The
   receiver processes each message on its own, and keeps no partial line
   or frame from one message to the next.

2. **A message carries many lines or frames.**  aprsc does not send each
   APRS-IS packet in a message of its own.

The first rule follows from the delivery order (see below): aprsc sends
its messages unordered, so if a line was split in two messages, the
receiver could not tell whether the second part arrived in the right
place.  With complete lines and frames in each message, there is nothing
to put together, and no state to keep between messages.

The second rule is because SCTP messages are expensive.  The kernel keeps
each queued message in a buffer of its own, and charges the send buffer of
the socket with the size of that buffer and its bookkeeping, not just the
data.  On Linux 5.15, each queued message was charged like this:

    message size    charged per message    data share
    100 bytes       ~990 bytes             10 %
    600 bytes       ~1500 bytes            40 %
    1400 bytes      ~2500 bytes            56 %
    4000 bytes      8192 bytes             49 %

A full feed of 200 packets per second of some 100 bytes each, one packet
per message, would use about 200 kB of kernel memory per second of data
queued.  TCP does not have this problem, since it appends small writes to
the buffers already queued.  SCTP keeps the messages apart, so aprsc packs
many packets in each.

Since a message can carry several packets, the message boundaries do not
tell where one packet ends and the next one begins.  That is done by
framing inside the messages, as on TCP: line terminators in APRS-IS, and
IS2 frames in IS2.


APRS-IS over SCTP
--------------------

A plaintext connection over SCTP works like one over TCP: the banner, the
login line and its `# logresp`, packets and comment lines, each line
terminated by CR LF.  The server accepts CR, LF, or both, as the end of a
line.

Each SCTP message carries one or more complete lines.  A client sends
whole lines in each message, and may send several of them in one; it
handles several lines in each message it receives.


IS2 over SCTP
----------------

IS2 is framed the same way on SCTP as on TCP:

    STX (0x02)
    a reserved byte, which must be 0
    the length of the message, 16 bits in network byte order
    an IS2Message, encoded with Protocol Buffers (aprsis2.proto)
    ETX (0x03)

A frame is at most 4096 bytes.  Each SCTP message carries one or more
complete frames, and each IS_PACKET frame carries many APRS-IS packets:
the server collects the packets of a round of its worker thread, up to
1200 bytes of them, in one frame.

On SCTP, the frame may look redundant, since a message boundary could
mark the end of an IS2Message.  It is kept, because:

* A message carries several frames, as explained above, and the length
  field separates them.

* The in-band upgrade (see IS2UPGRADE.md) switches a plaintext connection
  to IS2 on the same association.  After answering the hello, the server
  discards the plaintext lines the client sent before it saw the answer,
  such as the login line, until a line starts with STX: the first IS2
  frame.  The STX tells the frames from the plaintext lines.

* The framing is the same on every transport: TCP, TLS and SCTP use the
  same deframing code, and IS2 over UDP uses the same frames, one per
  datagram, with an HMAC trailer added (see IS2PEERING.md).

* The length field and the ETX catch a sender which gets the framing
  wrong, and the connection is closed instead of misreading the data
  that follows.

The framing costs 5 bytes per frame, which is little next to the
per-message overhead of SCTP.


How aprsc assembles the messages
-----------------------------------

The server assembles each outgoing SCTP message in the output buffer of
the client (8 kB by default), and sends the contents of the buffer with
one `send()`:

* At a low rate, 16 writes or less per 2-second scan of the client, each
  line or frame is sent right away, as a message of its own.  Few messages
  are queued at such rates, so the overhead does not add up.

* At a higher rate, more than 16 writes per scan, the client is switched
  to buffered writes: lines and frames are added to the buffer until it
  is half full, or until the end of the worker thread's round (at most
  30 ms), and then sent as one message.  The client is switched back to
  writing right away when the rate drops below 6 writes per scan.

A message is always sent whole: if the kernel does not take all of it,
the connection is closed.


The send buffer
------------------

The output buffer is only for assembling one message.  When the socket's
send buffer in the kernel is full, `send()` fails with EAGAIN, and the
server disconnects the client: it does not keep the data for later, since
adding more to the output buffer would merge the following lines into an
ever larger message.  The kernel's send buffer is what absorbs bursts of
traffic, and a slow client.

The default send buffer (`net.core.wmem_default`, 208 kB on Linux) holds
only a second or two of a full feed in small messages, so aprsc asks for
256 kB with `SO_SNDBUF`.  Linux doubles the request to account for its
own overhead, and caps it at twice `net.core.wmem_max`: 416 kB by
default, and 512 kB if the administrator raises `net.core.wmem_max` to
256 kB or more.  With a full feed of 200 packets per second, that holds
some 8 to 15 seconds of data when several packets go in each message, but
only about 2 seconds if each message carried a single packet.
Data the client has acknowledged leaves the send buffer, so it only fills
up when the client stops reading, or the network path is slow or loses
packets.


Delivery order
-----------------

aprsc sends its SCTP messages unordered (`SCTP_UNORDERED` in the default
send parameters of the socket) once the login is done, and turns off
the delay of small messages (`SCTP_NODELAY`).  An unordered message is
handed to the receiver as soon as it arrives, without waiting for the
messages sent before it.  On a path without losses the messages arrive
in the order they were sent, but a message which has to be retransmitted
can arrive after the ones sent after it.

The messages of the login handshake are sent in order: the banner, the
logresp, the `#is2 hello` line of an in-band upgrade, the IS2 server
signature and the login reply.  So that the first unordered messages can
not overtake a retransmitted message of the handshake, the server only
starts sending packets after the login when the peer has acknowledged
all of the handshake (the `SCTP_SENDER_DRY_EVENT` notification).  This
takes a round trip, or up to 200 ms more if the peer delays its
acknowledgements; packets in the meantime are not sent to the client,
as if it had logged in a moment later.  The flag is set by the sender of
each message, so a server only decides the order of the messages it
sends.

So the packets of one message stay in order, but the messages can be
reordered.  An APRS-IS server does not promise to keep the packets in
order anyway: packets arrive at it from many sources, and are passed on
as they come.  The rule of complete lines and frames in each message is
what makes the reordering harmless to the framing.


Receiving
------------

The server reads each message with one `recvmsg()`, into the input buffer
of the client (8 kB by default), and processes the lines or frames in it.
A message has to fit in that buffer, so a sender keeps its messages
below 8 kB.  The server's own messages fit in the output buffer of the
same size.

The server also receives the SCTP notifications of the association, and
logs them: the association coming up or being lost, the peer shutting
down, changes of the peer's addresses, and failed sends.


Monitoring
-------------

In the status JSON, clients and uplinks connected over SCTP have
`"proto": "sctp"`, and the totals have `sctp_bytes_rx`,
`sctp_bytes_tx`, `sctp_pkts_rx`, `sctp_pkts_tx`, `sctp_pkts_ign` and
`sctp_rx_errs`.  An IS2 connection over SCTP also has `"link": "is2"`,
like the IS2 connections over TCP.


Tests
--------

`TestSCTP` in `tests/go/functional/sctp_test.go` passes a load of packets
through a plaintext uplink over SCTP between two servers.  The tests in
`is2sctp_test.go` cover IS2 ports over SCTP, the in-band upgrade on
plaintext SCTP ports, a load of IS2 frames over SCTP, and an IS2 uplink
over SCTP.  They need a Linux host which can make SCTP connections, and
are skipped otherwise.
