
IS2 packet metadata on plaintext APRS-IS connections
-------------------------------------------------------

The IS2 protocol carries metadata with each packet, in addition to the
packet itself: so far the signal strength and the signal-to-noise ratio
measured by the receiver which heard the packet on the air.  A client
which speaks the traditional plaintext APRS-IS protocol can still supply
this metadata for the packets it feeds, without implementing IS2, by
sending it on a comment line just before the packet.

Work in progress, like IS2 itself: the set of keys may grow.


The metadata line
--------------------

The metadata goes on a line of its own, before the packet it belongs
to, on the same connection:

    #is2-meta key=value [key=value ...]

The line starts with `#is2-meta` and a space, in lower case, just like
that.  It is followed by one or more key-value pairs, separated by
spaces; a key and its value are separated by `=`, with no spaces around
it.  Values are numbers, such as `-97.5` or `300`.

The keys so far:

* `rx_rssi`: the received signal strength indication of the packet at
  the receiver, in dBm.  This is what LoRa radios report for a packet,
  and what LoRa APRS firmware logs: a negative number, the closer to
  zero the stronger, from around -30 for a nearby station down to -130
  or so for a packet barely decoded.  Decimals are fine; much firmware
  rounds to whole dBm.
* `rx_snr_db`: the signal-to-noise ratio of the packet at the receiver,
  in dB.  LoRa decodes packets well below the noise, so the value is
  often negative, down to about -20, and rarely above +10.
* `rx_modem`: the modem the packet was received with, as a number:
  1 for AX.25 packet radio, 2 for LoRa APRS, 3 for FLARM.
* `rx_bitrate`: the bit rate of the transmission in bit/s, as a whole
  number: 1200 or 9600 for packet radio, about 300 for the usual LoRa
  APRS setting.
* `rx_frequency`: the radio frequency the packet was received on, in
  Hz, decimals allowed; `433775000` for the European LoRa APRS channel,
  for instance.

Keys the server does not know are ignored, as are pairs without a key
or a value, so a client may send more than a given server understands.
A value which is not a number as a whole, such as `rx_snr_db=8dB`, is
ignored together with its key, and so is a value out of range: a modem
number not listed above, a bit rate of zero, a frequency of zero or
below.  The other pairs on the line still apply.

An example of a session, from the client's point of view, which logs in
and sends one packet with all the values:

    user N0CALL-10 pass 12345 vers myigate 1.0
    #is2-meta rx_rssi=-97.5 rx_snr_db=8.2 rx_modem=2 rx_bitrate=300 rx_frequency=433775000
    OH7LZB-9>APRS,WIDE1-1,qAR,N0CALL-10:!6028.51N/02505.68E>test

Any of the keys may be left out, when the receiver does not know the
value.


What the metadata applies to
-------------------------------

The metadata applies to the next packet sent on the connection, and to
that packet only.  It is consumed when the packet has been processed,
whether the packet was accepted or dropped, so a client sends a
metadata line before every packet which has metadata, and nothing
before the packets which have none.

A metadata line waits for the next packet, so a line sent after a
packet belongs to the one following it.

The metadata does not take part in duplicate detection, which looks at
the packet alone.  A packet which is dropped as a duplicate is dropped
together with its metadata, and the copy which went through first is
the one forwarded, with the metadata it came with.


Where the lines are accepted
-------------------------------

The metadata lines are accepted on the ports which accept filter commands,
those of the `igate`, `userfilter` and `clientonly` types, over the TCP
connection the client logged in on.  They are not accepted on full feed
ports, over UDP (the UDP submit port, or the UDP return path of a TCP
login), or in HTTP uploads.

The server accepts them when it has been built with IS2 support
(`./configure --enable-is2`).  Without it, and on other APRS-IS server
software, the line is a comment like any other, which servers ignore,
so a client may send the metadata lines regardless of what it is
connected to.


What happens to the metadata
-------------------------------

The server attaches the values to the packet as the `rx_rssi`,
`rx_snr_db`, `rx_modem`, `rx_bitrate` and `rx_frequency` fields of the
IS2 `ISPacket` message (see `aprsis2.proto`), exactly as if the packet
had come in over IS2 with them.  From there
they travel with the packet to IS2 clients and over IS2 peer links, for
as many IS2 hops as the packet makes.  Plaintext connections have no
room for them: a plaintext client receives the packet without the
metadata, and the metadata line itself is never forwarded to anyone.
When a packet crosses a plaintext link between servers, the metadata is
lost for good.

An IS2 client receiving the packet of the example above finds, in the
`ISPacket`, the packet line with the q construct the server added, and
the fields `rx_rssi` set to -97.5, `rx_snr_db` to 8.2, `rx_modem` to
`MODEM_LORA_APRS`, `rx_bitrate` to 300 and `rx_frequency` to 433775000.
