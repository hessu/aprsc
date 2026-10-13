
Upgrading a plaintext APRS-IS connection to IS2
--------------------------------------------------

A client can switch a connection on an ordinary plaintext APRS-IS port to
the IS2 protocol, without a separate IS2 port, and without knowing in
advance whether the server supports IS2.  It asks for the upgrade on a
comment line before its login line, which servers that do not know of IS2
ignore: the client then logs in on plaintext as usual, with no extra
round trip.

Work in progress, like IS2 itself.


The capability line
----------------------

A server which offers the upgrade sends a second line after its banner:

    # aprsc 2.1.20
    #is2 v=1

`v` lists the versions of the upgrade it supports, separated by commas.
Further `key=value` pairs may follow in later versions, and must be
ignored when not known.  Clients ignore comment lines from the server, so
the line is harmless to clients which do not know of IS2.  It also lets a
monitoring tool find the servers and ports which support IS2 by reading
the first two lines, without logging in.

The line is advisory: a client may send the hello regardless of it.


The hello
------------

To upgrade, the client sends a hello, followed by its normal login line
right away, without waiting for an answer:

    #is2 hello v=1
    user N0CALL pass 12345 vers myclient 1.0

`v` lists the versions the client supports, separated by commas
(`v=1,2`).  Like on the `#is2-meta` lines (see IS2PLAIN.md), the line
consists of space-separated `key=value` pairs after `#is2 hello`; keys
which are not known are ignored, so that later versions can add them.

The server answers in one of two ways:

1. A server which does not know of IS2, or does not offer the upgrade,
   ignores the hello as a comment, and answers the login line with the
   usual `# logresp`.  The connection continues on plaintext.  This is
   also the answer if the server does not support any of the versions
   offered.

2. A server which upgrades the connection answers with the version it
   picked, the highest one it supports from the list:

        #is2 hello v=1

   This is the last plaintext line from the server.  It is followed by
   IS2 frames, starting with the server signature, as on an IS2 port.
   The server discards the login line and any other plaintext lines
   from the client until an IS2 frame starts, with the STX (0x02) byte
   at the beginning of a line.  The client then logs in with an IS2
   login request, as on an IS2 port.

So the client knows which way it went from the first line after the
banner and the capability line: `#is2 hello` or `# logresp`.

The hello is only accepted before the login.  After it, `#is2 hello` is
an ordinary comment.

The passcode in the login line goes over the connection in plaintext
before the upgrade, as it does on a plaintext connection.  On a TLS
port, the whole exchange is inside TLS.


Where the upgrade is offered
-------------------------------

A server built with IS2 support (`./configure --enable-is2`) offers the
upgrade on its plaintext client ports over TCP, TLS and SCTP, of all
the port types (fullfeed, igate, clientonly, dupefeed). The upgraded
connection works like one on the corresponding IS2 port type.

The upgrade is not offered, and the hello is ignored:

* on ports configured with the `noIS2upgrade` option
* when `quirks_mode` is enabled for all clients: some clients are
  picky about what they receive, and might not like an extra line
* when a `fake_version` is configured, since the server is then
  pretending to be some other software

The status JSON tells whether the upgrade is offered on each listener
(`is2_upgrade`), and the clients which have upgraded have `"link": "is2"`
in the client list, like the clients of IS2 ports.
