
IS2: the binary APRS-IS protocol
===================================

IS2 is a second-generation protocol for the APRS-IS, carrying the
packets in Protocol Buffers messages instead of text lines.  The
packets themselves are the traditional APRS-IS lines, so IS2 and the
plaintext protocol interwork: a server bridges between them, and a
client sees the same packets on either.  What IS2 adds is a place for
information which the plaintext protocol has no room for, and a
link-level framing with sequence numbers and signatures.

aprsc implements IS2 when built with `./configure --enable-is2`.  This
document gives an overview of the protocol, its benefits and its costs;
the details are in the documents it refers to, and the message
definitions in `src/aprsis2.proto`.

Work in progress.  The protocol is not finished, and it may still
change.


Why a new protocol
---------------------

The plaintext APRS-IS protocol is a stream of lines, one packet per
line, and a login line and a few `#` comment lines around them.  It has
served since the 1990s, and it is simple to implement, but the line
format is full: everything on the line is the APRS packet, and anything
added to it would be taken for part of the packet by the receiver.
This has consequences:

* The receiver of a packet on the air knows the signal strength, the
  modem and the frequency, but there is no way to pass them on.  LoRa
  APRS receivers in particular have this information, and services
  which map coverage would like to have it.
* A packet carries no record of its route.  The q construct names the
  igate, or the first server, at best, and a client can format a packet
  so that it does not point to the client at all.  Tracking down the
  source of bad packets means reading the logs of each server in turn.
* The time a packet entered the network is not known, so the latency of
  the network cannot be measured, and a packet looping around the
  network for longer than the dupe check window of 30 seconds cannot be
  told from a new one.
* The login reply is an English sentence.  A client cannot tell why its
  login failed without matching the text, and cannot show a message in
  the user's language.
* The UDP peering between core and hub servers puts one packet in each
  UDP datagram, with nothing else.  There is no sequence numbering, so
  packet loss cannot be measured, and no authentication, so packets can
  be injected by spoofing the source address of a peer.
* Additions to the protocol can only be made as new comment lines, and
  in practice some clients and servers expect exactly the lines they
  have always seen, so even those are risky to add.

IS2 is designed to fix these, while keeping the APRS packets as they
are, so that the plaintext network and the IS2 network are one network.
It also allows adding new fields later without breaking interoperability.


Features
-----------

### The packets stay the same

An IS2 `ISPacket` carries the APRS-IS packet line as it is, with its q
construct, in a bytes field; around it are the metadata fields.  A
server receiving a packet from a plaintext client sends it to IS2
clients with metadata, and a packet from an IS2 client to a plaintext
client with the metadata stripped.  Dupe checking, filtering and
messaging work on the line, as before, so an IS2 client and a plaintext
client with the same filter get the same packets.

### Received entries: the route of a packet

Every IS2 server appends a Received entry to each packet it accepts,
much like the `Received` headers of an email: its own server id, the
login of the client or the id of the server it received the packet
from, how that client was verified (read-only, APRS passcode, or a
certificate), and the client's IP address.  A packet carries the newest
5 entries, so its length stays bounded if it loops, and the entry of
the last server is always there.  A packet which comes back to a server
whose entry it already carries is recognised as a loop.

The addresses are personal data and are only passed to other servers;
they are emptied from packets sent to clients.  See
[IS2RECEIVED](IS2RECEIVED.html).

### The time a packet entered the network

The first server to see a packet stamps it with the time of reception,
in milliseconds.  From the stamp, servers measure the latency of the
network, warn about links whose clocks are off, and optionally drop
packets which have been in the network for too long: the dupe check
closes loops within its 30-second window, and the age check closes
the ones which take longer.  See [IS2RECEIVED](IS2RECEIVED.html).

### Reception metadata

A packet can carry how it was heard on the air: the signal strength
in dBm, the signal-to-noise ratio in dB, the modem type (AX.25, LoRa
APRS, FLARM), the bit rate and the frequency.  An IS2 igate sets the
fields directly, and a plaintext igate can set them by sending an
`#is2-meta` line before the packet, which servers without IS2 ignore
as a comment.  The values travel with the packet over IS2 links; a
plaintext link drops them.  See [IS2PLAIN](IS2PLAIN.html).

### Login with machine-readable results

The server opens an IS2 connection with a server signature giving its
id, software and version, and a list of optional features it
supports.  The client logs in with a login request, and the login
reply carries the result, how the client was verified, a reason code
when the login failed (invalid passcode, certificate required,
certificate mismatch, username not allowed, and so on), an English
message, and the features agreed on.  A client can act on the reason
code and show a message in its own language.

### Filters as parameters

The filter of a connection is set with a parameter message carrying a
request id, and the server answers with the value it applied, or a
failure, with the same id, so a client can match replies to requests
and need not parse the `# filter ... active` comment line.

### Keepalives with round-trip times

Either end sends a ping with a request id and optional data, and the
other returns it as a reply unmodified.  The server measures the
round-trip time of its pings and shows the average and the maximum of
the last ones in its status, for clients, uplinks and UDP peers alike.

### Typed duplicates

On a dupe feed port, the duplicates suppressed by the dupe check are
sent as packets of the type `IS_PACKET_DUPLICATE`, so a receiver can
tell them from the packets which went through, without a separate
connection for each.

### Framing with a length

An IS2 frame on TCP, TLS and SCTP is an STX byte, a reserved byte, a
16-bit length, the Protocol Buffers message, and an ETX byte.  The
receiver knows how long a frame is before reading it, instead of
scanning for a line terminator, and a frame can carry binary data.
The message is a single container, `IS2Message`, which holds one
control message or a list of packets, so that in the common case a
server can pass the packets on without decoding them.

### Signed, numbered UDP peering

Between core and hub servers, IS2 frames over UDP carry a sequence
number and a signature: an HMAC-SHA256 made with a key shared by the
peer group, with a key identifier so that keys can be rotated, and a
timestamp which keeps a captured frame from being replayed later.  A
frame which is not signed with a configured key, or is too old, is
dropped, so peer traffic cannot be spoofed.  The sequence number
counts the frames lost on a link, allowing for reordering, and the
loss is shown per peer in the status.  The protocol is negotiated with
a signed handshake in comment packets, so a peer group can be upgraded
one server at a time: peers without a common key, or without IS2,
stay on plain UDP, and a peer which stops answering pings falls back
to plain UDP.  See [IS2PEERING](IS2PEERING.html).

### In-band upgrade of a plaintext connection

A client does not need a separate IS2 port, nor to know in advance
whether the server supports IS2.  It sends an `#is2 hello` comment
line before its login line; a server without IS2 ignores it and
answers the login as usual, and a server with IS2 answers with the
version it picked and switches the connection to frames.  Either way
the client knows from the first line it gets, without an extra round
trip.  See [IS2UPGRADE](IS2UPGRADE.html).

### Everywhere the plaintext protocol goes

IS2 ports come in the same types as plaintext ports (`is2fullfeed`,
`is2igate`, `is2dupefeed`), over TCP, TLS and SCTP, and an uplink can
be configured to use IS2 to a server which has it.  The live upgrade
of aprsc keeps IS2 connections open across a software upgrade, like
plaintext ones.  The status JSON tells which listeners accept IS2 and
offer the upgrade, and which clients are on IS2.

### Extensibility

New fields can be added to the messages without breaking existing
receivers, which skip the fields they do not know.  Optional features
are negotiated by name at login, and the UDP peering handshake and
the upgrade hello carry version lists and key-value pairs, so later
versions have room to grow without another flag day.


Why Protocol Buffers
-----------------------

The Protocol Buffers encoding was chosen for the messages for these
reasons:

* **Small overhead, binary encoding.**  A field costs a one-byte tag
  and, for strings and bytes, a length; integers are varints, so a
  small number takes one byte, and a field left at its default value
  takes nothing at all.  A packet with no metadata costs a few bytes
  more than the line with its CRLF.
* **Binary clean.**  Raw packets, IP addresses and hashes go in bytes
  fields as they are, with no escaping and no line terminator to
  avoid, so the framing never has to guess where a packet ends.
* **Extensible without a flag day.**  Fields can be added, and
  receivers skip the ones they do not know.  Field numbers, not names,
  go on the wire, so renaming a field costs nothing, and a field can
  be reserved and retired.  The `received` list, `received_time` and
  the reception metadata were added to `ISPacket` this way, and older
  IS2 receivers ignore them.
* **A schema which is also documentation.**  `src/aprsis2.proto` is a
  short, readable definition of every message and enum, with comments,
  and it is the single source for the C code of aprsc and the Go code
  of the test suite.  The compilers generate the encoders, decoders and
  types, so the two sides cannot drift apart, and the same file gives a
  client written in Python, Java, C#, JavaScript or Rust a working
  implementation of the messages.
* **Enums for machine-readable results.**  Login results, verification
  levels, modem types and message types are enums with stable numbers,
  so a client compares a number instead of matching an English string.
* **Proven in large deployments**, with libraries in every common
  language, and well documented:
  https://protobuf.dev/overview/

The encoding does not come for free: a receiver has to decode a frame
before it can see the packet, where a plaintext receiver reads it from
the line.  aprsc decodes each incoming frame once, in the worker which
received it, and encodes each outgoing packet once per worker, as
described below.


Performance: the costs
-------------------------

IS2 carries more information than the plaintext protocol, and the
information takes bytes on the wire and in memory.  The figures here
are for the current implementation and the sizes of the structures in
aprsc; they describe the order of magnitude, not exact values.

### Bytes on the wire

A plaintext packet costs its line plus CRLF.  An IS2 packet costs the
line plus:

* about 7 bytes of Protocol Buffers tags and lengths for the packet
  element and its type;
* about 7 bytes for `received_time`;
* 20 to 40 bytes for each Received entry: two callsigns, the
  verification status, and 4 or 16 bytes of address when it is
  present.  A packet which has crossed two or three IS2 servers
  carries two or three entries; the limit is five;
* 5 to 10 bytes for each reception metadata field, when the packet
  has them;
* per frame, shared by the packets in it: a 5-byte frame head and
  tail, a few bytes for the message type and sequence number, and on
  UDP peer links a 38-byte signature trailer.

So a typical 100-byte packet on a link between servers, with two or
three entries, grows by 70 to 130 bytes, roughly doubling the
bandwidth of a full feed.  A packet sent to a client has the addresses
emptied from its entries, which saves 4 to 16 bytes per entry, and a
packet which has not passed through an IS2 server has no entries at
all.  Servers which only see other servers can be configured not to
record addresses in their entries, or not to add entries, to cut the
cost on their links.

### Memory

Each packet buffer in aprsc has a fixed header followed by the packet.
With IS2, the header holds the Received entries of the packet in a
compact form, 5 entries of 38 bytes, and the decoded `ISPacket`
fields.  On a 64-bit build, the header is 448 bytes with IS2 and 248
bytes without, so a packet buffer, of which the server keeps enough
for the dupe check window and the slowest client, is about 200 bytes
larger.  At 200 packets per second and a 30-second window, that is
some 1.2 MB more.  The entries are stored in this compact form, not as
Protocol Buffers structures, and serialized only when the packet goes
out to an IS2 link, so the cost per packet is the fixed size, with no
allocations.

Each IS2 client has an output buffer of its own of about 1.3 kB, where
the frame being built waits for the end of the worker's round, and each
worker has two encoding caches of about 1 kB for the encoded forms of
the packet in hand, with and without addresses.  Plaintext clients pay
none of this.

### Processing

Each incoming frame is decoded once by the worker which received it;
the Received entries are validated and copied into the packet buffer,
and the packet line goes through the same parsers as a plaintext
packet.  Each outgoing packet is encoded at most twice per worker, in
the full and the redacted form, and the encoded bytes are copied to
the buffers of all the IS2 clients which get it.  The encoding of an
`IS2Message` around the packets is done by hand, from the field
numbers of the schema, which are checked against the generated code at
start-up, so no intermediate message structure is built per client.
The UDP peer frames additionally cost an HMAC-SHA256 computation per
frame sent and received.


Performance: the benefits
----------------------------

### Fewer datagrams and system calls on UDP peering

The plaintext UDP peering sends one datagram per packet, so a hub
exchanging a full feed with a peer group sends and receives one
`sendto()` or `recvmsg()` system call, one UDP datagram and one IP
packet per APRS packet per peer.  At the packet rates of the APRS-IS,
this is already most of the cost of a core server, and at the rates of
the Open Glider Network, over 600 packets per second, it is a problem.

An IS2 peer link packs the packets of a worker's round into one frame,
up to 20 packets or 1200 bytes of encoded packets per datagram, and
sends them with one system call.  Under load a round collects several
packets, so the number of datagrams, system calls, signatures, and IP
headers per packet drops by about as many.  The per-datagram costs
(the 28 bytes of UDP and IP headers, the 38-byte signature, the frame
head and tail) are spread over the packets in the frame, which also
pays back part of the bytes the metadata adds.

The same packing applies to IS2 clients and uplinks on TCP, TLS and
SCTP: the packets of a round go out in one frame with one write.  TCP
would coalesce small writes by itself, but SCTP charges each message
separately, as described in [SCTP](SCTP.html), and TLS adds a record
per write, so one write per round is a real saving there.

### No scanning for line ends

A receiver reads the length of a frame from its head and reads that
many bytes, instead of scanning the input for CR and LF, and the
packet comes out of the frame as a bytes field with its length known.
There is nothing to escape and nothing to unescape.

### Loops closed sooner, bad links found

The Received entries and the `received_time` stamp let a server
recognise a looping packet before the dupe check does, measure the
latency of the network per link, and find a peer or uplink whose clock
is off, where the plaintext protocol gives it no information at all.
The sequence numbers on UDP peer links give the loss figures per link
that plain UDP cannot, so a lossy path is seen in the status instead
of as unexplained missing packets.

### Spoof-proof peering

Signed UDP frames close the hole that lets anyone with a spoofed
source address inject packets into a peer group.  This is not a
speed-up, but it removes a class of trouble which costs operators time.


Related documents
--------------------

* [IS2PEERING](IS2PEERING.html): the UDP peering handshake and the
  signed data frames
* [IS2UPGRADE](IS2UPGRADE.html): the in-band upgrade of a plaintext
  connection to IS2
* [IS2RECEIVED](IS2RECEIVED.html): the Received entries and the
  `received_time` stamp
* [IS2PLAIN](IS2PLAIN.html): reception metadata from plaintext clients
* [SCTP](SCTP.html): how IS2 frames go in SCTP messages
* `src/aprsis2.proto`: the message definitions
