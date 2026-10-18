
APRS-IS UDPPEER2 protocol
----------------------------

This is an attempt to document a new UDP based protocol for passing APRS-IS
packets between hub servers.

Work in progress. Not finished.


Background
-------------

Currently APRS-IS packets are transmitted between peers (between aprs2.net
hubs and aprs.net core servers) in UDP frames, simply by putting each 1-line
packet inside an UDP frame without any other information.  Since there is no
sequence numbering, there is no statistical information on any possible
packet loss, or other issues, for peer sessions.  There is also no
authentication, so UDP frames can be injected by spoofing the source IP
address.  The current protocol is not extensible, additional information can
not be added before or after the packet.  Therefore, a new protocol would be
useful for adding link-layer information and future extensions.

The Open Glider Network has started to use aprsc, with packet rates
exceeding 600 packet/second.  Running peering sessions with UDP would cause
unnecessarily high packet rates, as opposed to TCP which can coalesce
multiple APRS packets into a single TCP segment and IP packet.  On the other
hand, it would be good to set up the UDP full mesh peering between their
nodes, so that the core interconnections would be more stable without TCP
backoff issues.


Proposal
-----------

Packets are transmitted over UDP using Protocol Buffers encoding, but only
after it has been confirmed that the other peer can process them.  A
handshake protocol for detecting capable peers is documented.  So far, only
the handshake protocol is documented here (work in progress).

Before a successful handshake is completed, peers can already transmit
packets using the old plain UDP format.  The handshake is an opportunistic
upgrade method to the new protocol: if it doesn't happen, old protocol
is used as before.

Once an upgrade to the new protocol is made, a ping-pong polling
routine is started: the server sends an IS2 keepalive ping to the peer
at the keepalive interval of 20 seconds, in a signed frame of its own
which carries a sequence number like the packet frames, and the peer
answers it the same way. The round-trip times of the last pings are
shown in the status of the peer. Should the replies stop for more than
5 minutes, the sender reverts to the old protocol, in case the peer
would be downgraded to software which does not understand v2 binary
packets, and starts proposing the upgrade again as at the beginning.

The Protocol Buffers encoding is chosen due to the following features:
* Small overhead, binary encoding
* Binary clean, can transport binary byte streams such as raw packets
  and hashes
* Extensible - fields can be added without breaking existing receivers
* Has a simple, lean language to document and synchronize the protocol
  structure and compilers to transform the specification to code
  in various languages (C, C++, C#, Java, Python, Go...)
* Proven to work in large deployments (Google & friends)
* Fairly good documentation:
  https://developers.google.com/protocol-buffers/docs/overview


Handshake
----------

The handshake packets are comment packets, starting with a '#',
so that they can be safely ignored by older server versions.

A handshake packet is `#is2 peer`, the packet type (`hello` or `ok`), and
space-separated `key=value` pairs:

* `v`: the protocol versions: in a hello, a comma-separated list of the
  versions the sender supports, and in an ok, the one version picked
* `id`: the Server ID of the sender
* `vers`: the software name and version of the sender, as one word
  (`aprsc/2.1.20`), for logging
* `token`: a random token, unique to each hello, and returned in the ok;
  printable characters, at most 64
* `key`: the identifier of the PeerGroupKey the packet is signed with
* `ts`: the time of sending, in seconds since 1970-01-01 UTC
* `mac`: the signature, an HMAC-SHA256 in 64 hexadecimal digits, of all of
  the packet before the space preceding `mac=`, made with the key

All of them except `vers` are required, and `mac` must be the last one;
the others can come in any order.  Keys which are not known must be
ignored, so that later versions can add them, and words without a '='
are skipped.  This is the same key-value format as used on the `#is2-meta`
lines of plaintext clients (see IS2PLAIN.md), and the prefix follows the
`#is2` lines used to upgrade plaintext client connections.

The signature uses the same keys as the data frames (see below): a
handshake packet is only accepted if it is signed with a key configured
for the peer group, and its time is within 10 seconds of the receiver's
clock.  A handshake can therefore not be spoofed, and IS2 is only enabled
with a peer which shares a key, so that a peer with no common key stays on
plain UDP instead of switching to frames which would be dropped.

1. Peer 1 (SERVER-I1) transmits an UDP comment handshake packet,
   saying which versions of the protocol it supports, giving its Server
   ID, app version, and a random token unique to this packet.

        #is2 peer hello v=2 id=SERVER-I1 vers=aprsc/1.2.3.4 token=hq736t7q63 key=1 ts=1760000000 mac=4f1c...

2. Peer 2 (SERVER-I2), upon receiving the packet from a preconfigured
   peer's IP address, with that peer's Server ID and a valid signature,
   picks the highest version it supports from the list, and replies with
   it, returning the random token. If the peer does not support any of the
   versions offered, or does not understand the handshake at all, it will
   not reply.

        #is2 peer ok v=2 id=SERVER-I2 vers=aprsc/2.3.4 token=hq736t7q63 key=1 ts=1760000000 mac=9a07...

3. Peer 1 (SERVER-I1), upon receiving this packet, with a valid signature,
   the correct token and one of the versions it offered, knows that Peer 2
   will understand that version of the protocol, and can start sending its
   data packets to it.

For compatibility with future versions, a peer supporting versions 2 and 3
sends `v=2,3`, and a peer which only supports version 2 replies with `v=2`.
Further capabilities can be negotiated by adding keys: the hello offers
them, and the ok tells which of them were accepted.

Version 1 is the plain UDP peering without a handshake.

Peer 2 will individually send it's own 'hello' packet (step 1) to confirm
that Peer 1 does v2.  Receiving a 'hello' packet does not by itself enable
v2 in the other direction: only a signed 'ok' with the token of our own
hello does.

A node can transmit an initial "hello" packet immediately after starting up,
and at regular intervals (once per 10 minutes?) when receiving non-v2
packets from a peer.  This is to ensure that a peering link will bump up to
v2 protocol eventually, if one node is upgraded, or if the initial handshake
packets are lost.


Data frames
------------

After the handshake, each UDP datagram carries one IS2Message, as
defined in aprsis2.proto, in a frame followed by an authentication
trailer:

    0x02 (STX)
    24-bit big-endian length of the message
    IS2Message, Protocol Buffers encoded
    0x03 (ETX)
    0x05, marker: an authentication trailer follows
    8-bit key identifier, 0-15
    32-bit big-endian unsigned Unix time of the sender
    HMAC-SHA256, 32 bytes

The HMAC is calculated over everything before it: the frame, the
marker, the key identifier and the timestamp.  The keys are shared
secrets configured on all peers of a group (PeerGroupKey), the key
identifier selects which one was used, so that keys can be rotated.
The sender signs with the key selected by PeerGroupKeyTransmit.  A
server which has no such key configured can not sign frames, so it does
not propose the upgrade to its peers, nor accept it when a peer
proposes: its peer links stay on plain UDP.

A receiver drops a frame if it does not carry a trailer, the key
identifier is not configured, the HMAC does not verify, or the
timestamp differs from the receiver's clock by more than 10 seconds.
The timestamp prevents replaying captured frames later: a replay within
the window is dropped by the duplicate packet filter, which remembers
packets for longer than that.  Peers need to keep their clocks in sync,
with NTP or similar.

Frame loss
-----------

Every frame carries a sequence number in the IS2Message, counted up by
one per frame sent to that peer, pings included.  It is only used for
detecting lost frames: the receiver keeps the newest sequence number
received and a window of the 64 frames behind it, and a frame is counted
as lost when it is still missing by the time the window has moved past
it, or when nothing newer has arrived for 10 seconds.  So frames
reordered on the wire within the window are not counted as lost, a burst
of lost frames of any length is counted in full when the next frame
after it arrives, and the number may wrap around.  A frame which arrives
after being counted lost is counted as late, and a frame received twice
as a duplicate.  A peer which restarts begins the numbering from zero
again, which the receiver recognises and starts over from; when the
first frames of the new numbering were missed, the restart is recognised
once a window's worth of frames in sequence have arrived far behind the
old numbering.

The counts are shown per peer in the status: the received, lost, late
and duplicate frames since the server started, and the received and
lost frames over the last five minutes, from which the status page
shows the short-term and long-term loss percentages.


