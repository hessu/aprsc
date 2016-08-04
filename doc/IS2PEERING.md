
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
routine is started. Should it fail for more than 5 minutes, the sender
must revert to the old protocol, in case the peer would be downgraded
to software which does not understand v2 binary packets.

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

1. Peer 1 (SERVER-I1) transmits an UDP comment handshake packet,
   saying it supports "v2" protocol, gives its Server ID, app version,
   and a random token unique to this packet.

   \# PEER2 v 2 hello SERVER-I1 vers aprsc 1.2.3.4 token hq736t7q63

2. Peer 2 (SERVER-I2), upon receiving the packet from a preconfigured
   peer's IP address, replies, saying it also can do the "v2" protocol,
   and returns the random token. If the peer does not understand v2,
   it will not reply.
   
   \# PEER2 v 2 ok SERVER-I2 vers aprsc 2.3.4 got hq736t7q63

3. Peer 1 (SERVER-I1), upon receiving this packet, with the correct token,
   knows that Peer 2 will understand the v2 protocol, and can start
   sending v2 data packets to it.

For compatibility with future versions, if Peer 2 receives a "v 4 hello"
packet, and does not understand v4 yet, it should respond with a "v 2 ok"
packet.  Any additional keys and values after the token must be ignored, so
that further versions can add parameters in the hello packet.

Peer 2 will individually send it's own 'hello' packet (step 1) to confirm
that Peer 1 does v2.  Receiving a 'hello' packet does not necessarily
mean that the peer would do v2 - the packet could have been spoofed.

A node can transmit an initial "hello" packet immediately after starting up,
and at regular intervals (once per 10 minutes?) when receiving non-v2
packets from a peer.  This is to ensure that a peering link will bump up to
v2 protocol eventually, if one node is upgraded, or if the initial handshake
packets are lost.


