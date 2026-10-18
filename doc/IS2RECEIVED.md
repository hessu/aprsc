
IS2 Received entries: where a packet has been
------------------------------------------------

A client connects to an APRS-IS server anywhere, and the packets it
sends spread through the network.  When one of them arrives from a
server somewhere else, the traditional APRS-IS packet format does not
say which server took it in, or from whom: the q construct names the
igate or the first server at best, and a client can format a packet so
that it does not point to the client at all.

IS2 carries this information with the packet, much like the `Received`
headers of an email.  Every IS2 server which receives a packet appends
an entry naming itself, the client or server it received the packet
from, how that client was verified, and the client's address.  A packet
thus carries the route it took, and the first server to see a packet
stamps it with the time it entered the network.

Work in progress, like IS2 itself.


The entries
--------------

The `ISPacket` message of `src/aprsis2.proto` has a repeated field
`received` of `IS2Received` messages, oldest first, newest last.  An
entry has:

* `server_id`: the server which received the packet, as it identifies
  itself in its server signature; `T2FINLAND`, for instance.
* `username`: the login of the client the server received the packet
  from, or the server id of the server it came from on an uplink or a
  peer link.
* `verified`: how the server verified that client, as a
  `VerificationStatus`: `NONE` for a read-only client, `WEAK` for the
  APRS passcode, `STRONG` for a client certificate or the equivalent.
  A server's uplinks and peers are `WEAK` unless they were verified
  with a certificate, or the link is an IS2 UDP peer link, whose
  frames are signed with a shared key (see `IS2PEERING.md`).
* `client_addr`: the IP address the packet was received from, as 4
  bytes for IPv4 or 16 bytes for IPv6, in network byte order.  An IPv4
  client of a listener bound to an IPv6 address is recorded as the 4
  bytes.  The field is empty when the server did not record the
  address, or when it was removed before the packet was passed on, see
  below.

An entry is added when a packet is accepted, so a packet dropped by the
server does not get one.  The entry is added to packets arriving from
clients, uplinks and peers alike, on IS2 and plaintext connections: a
packet from a plaintext client carries the entries from the first IS2
server on.  Plaintext clients never see the entries, as the plaintext
format has no place for them.

The server stores the entries of a packet in a compact form of its own
and serializes them when the packet goes out, so the entries cost the
server very little; the length they add to a packet on the wire is the
main cost, about 20 to 40 bytes per entry.


How many are kept
--------------------

A packet carries at most 5 entries, the newest ones.  When a server
adds its entry to a packet which already has 5, the oldest is dropped.
The limit holds the length of a packet down should it loop around the
network, and makes sure the entry of the current server is always
there, whatever a packet arrived with: a client cannot fill the list up
to keep a server from adding its entry.

A normal route is short.  A packet from a client to a Tier 2 server,
from there to a core server, and on to a client of another Tier 2
server has three entries when it arrives: the first Tier 2 server
naming the client, the core server naming the first Tier 2 server, and
the second Tier 2 server naming the core server.

A packet whose entries already name the server receiving it has been
here before: it has looped, or come in twice over different links.
The server counts these in `is2_received_loops` in the `totals` of its
status, and passes the packet on; the dupe check drops the duplicates
as before.


Trust
--------

An entry is only as trustworthy as the server which added it.  A client
may send a packet which already has entries, as it might have forged
anything else in the packet, and the servers keep them: a Tier 2 server
connecting to a hub is a client of the hub, and the entries it sends
are real.  Trust the entries from the newest backwards, as far as the
first server you do not trust.  The newest entry is always the one of
the server you got the packet from.

Entries which are not well formed are dropped on arrival, and counted
in `is2_received_invalid`: a server id or a username which is empty,
longer than 9 characters (the maximum length of a callsign) or not
printable ASCII, a verification status
not in the enum, or an address of some other length than 4 or 16 bytes.
The packet itself, and its other entries, are kept.


Privacy of the addresses
---------------------------

The address of a client is personal data, and the entries are passed
to other servers only with it.  A packet sent to a client, on any kind
of port, has the `client_addr` of every entry emptied.  The uplinks and
the peers of the server get the entries as they are, so that the
addresses are available to the operators of the network for tracking
down the sources of abuse.  Clients which could be authorised to see
the addresses are not implemented yet.

A server which only has other servers connected to it, such as an
aggregator or a hub, does not need to record the addresses of those
servers in every packet, and can be configured not to: its entries
then have an empty `client_addr`, while the entries of the packets it
receives are passed on unchanged, addresses included.  A server can
also be configured not to add an entry at all, while still keeping,
limiting and passing on the ones which come in.  These are settings for
the operators of such servers; the other servers run with the defaults,
and they are not documented in CONFIGURATION.md on purpose.


The time of reception
------------------------

The `received_time` field of the `ISPacket` is the time the packet
entered the APRS-IS, in milliseconds since the Unix epoch.  The first
server to receive a packet sets it, when it is not set already, and the
servers after it pass it on as it is, so the stamp survives the
dropping of the oldest entries.  It is the basis of the measurement of
the latency of the network, and of the dropping of old packets.

A server compares the stamp of every IS2 packet it receives with its
own clock.  A packet stamped more than 30 seconds in the past has been
in the network for too long, in a loop, or came through a server whose
clock is off; one stamped more than 30 seconds in the future comes from
a server whose clock is ahead.  The server counts these in
`is2_received_too_old` and `is2_received_too_new` in the `totals` of
its status, and logs a warning naming the link and the offset, at most
once a minute per link.  Dropping them is optional, off by default,
and shows up as the `is2_too_old` and `is2_too_new` rx errors of the
link when enabled.  The dupe check drops a looping packet within its 30
second window; the age check closes the loops which take longer.

A packet without a stamp, from a server which does not set one, is
never dropped by age, and gets stamped by the first server which does.
A packet which arrives with a stamp set by a client is not stamped
again: a client can only hurt its own packets with a bad stamp.
