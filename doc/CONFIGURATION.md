
Configuring aprsc
=================

Command line parameters and startup
--------------------------------------

aprsc understands a few command line parameters. On Ubuntu/Debian derived
systems these go to /etc/default/aprsc, on CentOS/Fedora they go to
/etc/sysconfig/aprsc, and on other systems they go in the init script
starting up the software.  Shown here are the settings installed by default
when installing aprsc from a binary package.

 *  `-u aprsc` - switch to user 'aprsc' as soon as possible
 *  `-t /opt/aprsc` - chroot to the given directory after starting
 *  `-f` - fork to a daemon
 *  `-e info` - log at level info (can be changed to "debug" for more verbose
    logging)
 *  `-o file` - log to file (can be changed to "stderr" for supervisord
    and debugging, or "syslog" for syslogd)
 *  `-r logs` - log file directory, log files are placed in /opt/aprsc/logs
 *  `-c etc/aprsc.conf` - configuration file location
 *  `-p /opt/aprsc/logs/aprsc.pid` - specify path to generated pid file, default path is logdir/aprsc.pid
 *  `-y` - try to parse the configuration, report success or error,
    quit after reading configuration. Useful for validating your
    configuration before restarting aprsc.

Since the daemon does a [chroot][chroot] to /opt/aprsc, all paths are
relative to that directory and the daemon cannot access any files outside
the chroot. The supplied startup script copies a couple of essential files
from /etc to /opt/aprsc/etc so that DNS lookups work (hosts, resolv.conf,
gai.conf, /nsswitch.conf).

aprsc refuses to run as root, but it should be started to root so that it
can do the chroot() dance and adjust resource limits as needed. When started
as root, it requires that the -u parameter is set to an unprivileged user.
Right after the chroot() it switches to the specified user to reduce the
damage potential. For security reasons it's a good idea to have a separate
unprivileged user account for aprsc. The official binary aprsc packages
automatically creates an "aprsc" user account and uses that in the
configuration.

aprsc can log to syslog too, but that will require bringing the
syslog socket within the chroot.

[chroot]: http://en.wikipedia.org/wiki/Chroot


On-line reconfiguration
--------------------------

aprsc has been designed from the start to support configuration reloading
without requiring a full software restart.  Many APRS server operators
value high uptimes and service availability, so server restarts should be
kept to a minimum.

With the known exception of `ServerId` and `FileLimit`, all parameters can be
adjusted without a restart.

To reload configuration, execute the `reload` option of the startup script.

On Ubuntu or Debian (systemd):

    sudo systemctl reload aprsc

On Ubuntu or Debian, old-fashioned:

    sudo service aprsc reload

On Centos (and others):

    /etc/init.d/aprsc reload

If there are any obvious problems with the new configuration, the startup
script will complain about that and skip the reconfiguration step.  The same
check will be executed before restarting, should you use the `restart`
option of the start script.

After executing an on-line reconfiguration you should always check the log
file for any errors or suspicious things happening after the reload. 
Strange things may happen and the pre-flight check might not catch all
errors in the configuration.


Configuration file format
----------------------------

Comment lines starting with the hash/number sign "#" are ignored.

String parameters containing spaces need to be enclosed in double quotes
("). Actually, any parameters may be enclosed in double quotes, but they are
unnecessary unless there are spaces in the parameter.

C-style backslash escapes are supported. If a literal backslash (\\)
character needs to be entered within a parameter, it must be escaped with
another backslash (\\\\). The source MarkDown file of this document contains
double backslashes so that the correct amount of backslashes will be shown
when the file is rendered as HTML.


Configuration file options in aprsc.conf
-------------------------------------------

### Basic server options ###

 *  ServerId NOCALL
    
    This is your unique server ID. Typically contains a callsign and
    an SSID.

 *  PassCode 0
    
    This is the passcode for your server ID. It is required if your server
    connects to upstream servers (Uplink configuration, see below).

 *  MyAdmin    "My Name, MYCALL"

    Your name and callsign.

 *  MyEmail email@example.com
    
    This is an email address where you, the server operator, can be
    contacted. It is not displayed on the status web page, but it goes to
    the machine-readable status.json file, so persons skilled in the art
    can find a way to contact you.

 *  LogRotate megabytes filecount

    If logging to a file (-o file), this option enables built-in log rotation.
    "LogRotate 10 5" keeps 5 old files of 10 megabytes each.

 *  MaxClients 500

    Maximum clients logged in to the server (not counting peers or uplinks).
    File descriptor limit must always be slightly larger than MaxClients
    (FileLimit >= MaxClients + 50 is a good figure), so that Uplink
    connections and access to local files work even when MaxClients is
    reached.

### Timers and timeouts ###

Timer settings assume the time is specified in seconds, but allow appending
one of s, m, h, d to multiply the given value by the number of seconds in a
second, minute, hour and day respectively. Examples:

 * 600 - 600 seconds
 * 600s - 600 seconds
 * 5m - 5 minutes
 * 2h - 2 hours
 * 1h30m - 1 hours 30 minutes
 * 1d3h15m24s - you get it?

And here are the contestants:

 *  UpstreamTimeout 15s

    When no data is received from an upstream server in N seconds,
    disconnect and switch to another server.
     
 *  ClientTimeout 48h

    When no data is received from a downstream client in N seconds,
    disconnect.


### Port listeners ###

The *Listen* directive tells aprsc to listen for connections from the network.
The basic syntax is:

    Listen socketname porttype protocol address port options...

 *  socketname: any name you wish to show up in logs and statistics. Quote
    it if it contains spaces ("Full feed").

 *  porttype: one of:
 
    - fullfeed - everything, after dupe filtering
    - igate - igate / client port with user-specified or fixed filters
    - udpsubmit - UDP packet submission port (8080)
    - dupefeed - duplicate packets dropped by the server
 
 *  protocol: either tcp or udp
 
 *  address: the local address to listen on. "::" for IPv6 `IN6ADDR_ANY`
    (all local IPv6 addresses), "0.0.0.0" for IPv4 `INADDR_ANY` (all local
    IPv4 addresses).  On Linux and Solaris, just put "::" here and it will
    handle both IPv6 and IPv4 connections with a single configuration line! 
    On FreeBSD and Windows, separate IPv4 and IPv6 listeners are needed.
    
 *  port: the local TCP or UDP port to listen on. Must be larger than 1023.
    aprsc drops root privileges quickly after startup (if it has them), and
    cannot bind privileged ports. If you need to support a low port such as
    23, see [TIPS](TIPS.html): "Providing access on low TCP ports"

 *  options: one more of:
 
    - filter "m/500 t/m" - force a filter for users connected here.
      If you wish to specify multiple filters, put them in the same "string",
      separated by spaces. The filter option only works with the igate port
      type.
    - maxclients 100 - limit clients connected on this port (defaults to 200)
    - acl etc/client.acl - match client addresses against ACL
    - hidden - don't show the port in the status page

If you wish to provide UDP service for clients, set up a second listener on
the same address, port and address family (IPv4/IPv6).

Example of normal server ports for Linux, supporting both TCP and UDP,
IPv4 and IPv6:

    Listen "Full feed"                fullfeed  tcp ::  10152 hidden
    Listen ""                         fullfeed  udp ::  10152 hidden

    Listen "Client-Defined Filters"   igate     tcp ::  14580
    Listen ""                         igate     udp ::  14580

    Listen "350 km from my position"  igate     tcp ::  20350 filter "m/350"
    Listen ""                         igate     udp ::  20350 filter "m/350"

    Listen "UDP submit"               udpsubmit udp ::  8080


### Uplink configuration ###

Uplink name type tcp address port

 *  name: a name of the server or service you're connecting to
 *  type: one of:
    - full - send a full feed to upstream
    - ro   - read-only, do not transmit anything upstream

If you wish to specify multiple alternative servers, use multiple Uplink
lines, one for each server. aprsc will automatically maintain a connection
to one of them.

Normally a single line for the 'rotate' address is fine - it will connect
to one of the servers in a random fashion and go for another one should
the first one become unavailable.

Here's a good configuration for connecting to the APRS-IS core:

    Uplink "Core rotate" full  tcp  rotate.aprs.net 10152


### Binding source address when connecting upstream (optional) ###

If your server has multiple IP addresses and you need to use a specific
source address when connecting to the upstream servers, the UplinkBind
directive will make your day. If not, just leave it commented out.

If you're using both IPv4 and IPv6 to connect out, enter two UplinkBind
directives, one with an IPv4 address and another with the IPv6 one.

    UplinkBind 127.0.0.1
    UplinkBind dead:beef::15:f00d


### HTTP server ###

aprsc can listen for HTTP requests on one or multiple TCP ports. Each HTTP
port responds to either status page queries, or takes in HTTP-based position
uploads, but not both. HTTP listener configuration directioves take an IP
address and a TCP port. Like the other Listeners, :: means "all addresses"
for IPv6, 0.0.0.0 means "all addresses" for IPv4. On Linux, Solaris and OS X
the :: IPv6 listener will also accept IPv4 connections happily, so you don't
usually need a separate IPv4 listener.

If you don't have a global IPv6 address (/sbin/ifconfig -a will not show an
IPv6 address with Scope:Global), using :: will not work, and you'll have to
use 0.0.0.0. The libevent2 http server which does HTTP address configuration
works like this.

The status page HTTP service is configured using HTTPStatus. Here's a
configuration example for the APRS-IS standard status port 14501:

    HTTPStatus 0.0.0.0 14501

The HTTP position upload service is configured using the HTTPUpload
directive. Here's an example for the APRS-IS standard position submission
port 8080:

    HTTPUpload 0.0.0.0 8080

Multiple HTTPStatus and HTTPUpload directives can be entered to listen on
multiple addresses or ports:

    HTTPUpload 127.0.0.1 8080
    HTTPUpload 10.73.88.99 8080
    HTTPUpload ::1 8080
    HTTPUpload f00d::beef:bac0:ca1f 8080


### HTTP status page ###

The status page can currently take one optional configuration parameter,
which enables displaying the admin's email address. If ShowEmail is set
to 0 (the default setting), the address is still available in the status.json
file which can be downloaded from the server, it's just not rendered on
within HTML page.

    HTTPStatusOptions ShowEmail=1


### Rejecting logins and packets ###

The following options allow the server operator to reject logins and packets
from specific callsigns.  Both options support 'glob' type wildcards ('*'
matches 0 or more any characters, '?' matches exactly one any character). 
The examples below are examples only - there is currently, by default, no need
to have the example callsigns blocked.

Separate multiple callsigns with spaces.

DisallowLoginCall rejects logins by the specified callsign. Callsigns which
are not valid callsigns on the APRS-IS (non-alphanumeric, too long, etc) are
rejected by default.

    DisallowLoginCall P1RAT* P?ROT*

DisallowSourceCall makes the server drop packets sent by the given source
callsign, even though they were injected at a different server.  The
following callsigns are dropped by default: N0CALL* NOCALL* SERVER*

    DisallowSourceCall P1RAT* P?ROT*


### Environment ###

When the server starts up as the super-user (root), it can increase some
resource limits automatically. When not starting as root these directives
have no effect.

If you wish to run a server which accepts a very large amount of users
(usually not necessary), the amount of connections a server can accept can
be limited by the operating system's limits for maximum open file
descriptors per process. The FileLimit directive can be used to adjust this
limit.

    FileLimit 10000

The FileLimit parameter cannot be adjusted by doing a reconfiguration after
startup, changing it requires a full restart. aprsc drops root privileges
after startup and cannot regain them later to adjust resource limits.


### Operator attention span qualification run ###

The example configuration file contains an invalid configuration directive
in the end for the purpose of making sure the operator actually reads
through the file and adjusts it to match his needs. If it caught you by
surprise, you should probably read through the whole configuration file with
some added thought, and cross-reference the configuration directives with
this document.

Think of this as the "brown M&M's test" by Van Halen, adapted for the APRS-IS.


Access list (ACL) file format
--------------------------------

Some directives in the main configuration can refer to ACL files. ACLs are
used to allow and deny connections based on the client's IP address.

If no ACL file is specified for a port listener, all connections will be
allowed. If an ACL file is configured, the default is to not allow any
connections unless an "allow" rule permits it.

Rules in an ACL file are processed sequentially, starting from the beginning. The
first `allow` or `deny` rule matching the address of the connecting client
is applied.

The first two following lines deny the `dead:beef:f00d::/48` subnet, and then allow the rest of the `dead:beef::/32` network around it. The third and fourth lines rules allow connections from 192.168.* except for 192.168.1.*, and last line allow connections from the host at 10.52.42.3. Without any further rules all other IPv4 and IPv6 connections are denied.

    deny  dead:beef:f00d::/48
    allow dead:beef::/32
    deny  192.168.1.0/24
    allow 192.168.0.0/16
    allow 10.52.42.3
  
If prefix length is not specified, a host rule is created (32 bits for IPv4,
128 bits prefix length for IPv6). To configure a rule that matches all
addresses you should specify a prefix length of 0 (::/0 for IPv6, 0.0.0.0/0
for IPv4).

If you want to specify an ACL file and allow any connection, you can use following ACL file content:

    allow 0.0.0.0/0
    allow ::/0
  
ACL files are read and parsed when aprsc starts or reconfigures itself, so
you need to do a configuration reload (on-line reconfiguration) after
changing the contents of an ACL file.

The same ACL file can be referenced from multiple main configuration
directives to reduce the amount of configuration files. For example, you
could have a single "allow.acl" file which would contain allow lines for
both IPv4 and IPv6 addresses, and the ACL can then be referred to from both
the IPv4 and IPv6 listeners.


Message Of The Day
---------------------

The MOTD feature allows you to push information on the status web page.  The
message is published by simply creating a HTML file named `motd.html` in the
web directory (typically `/opt/aprsc/web/motd.html`).  The MOTD will then be
automatically loaded to the top of the status page, and whenever it's
updated, it will be automatically refreshed within about 1 minute to anyone
having the status page open.

The motd.html file contents must be encoded in UTF-8, as that is what the
rest of the status page uses.  Be sure to select UTF-8 in your editor or
terminal application.  If you're only using plain English ASCII text and
HTML tags, this doesn't matter so much.  aprsc does not have a problem with
displaying Finnish or Japanese, or any other language, as long as the MOTD
is in UTF-8.

To remove the MOTD, simply remove motd.html or rename it to nomotd.html (or
something).  There is no need to restart aprsc to make changes in the MOTD.

To display consistent HTML messages, you might want to use the same
Bootstrap CSS classes as aprsc itself.  Here are some good sample contents -
you can paste these as-is to motd.html (without any other extra tags around
them - no `html` or `body` tags needed):

    <div class='row'>
    
    <div class="col-md-6 col-sm-12">
    <h4 class="text-info">Maintenance coming</h4>
    <p class="text-info">
    Informative message about upcoming maintenance or configuration change.
    Coloured accordingly.
    </p>
    </div>
    
    <div class="col-md-6 col-sm-12">
    <h4 class="text-success">Good news everyone!</h4>
    <p>This server has been upgraded to aprsc 2.1.</p>
    </div>
    
    <div class="col-md-6 col-sm-12">
    <h4 class="text-danger">Oops!</h4>
    <p class="text-danger">
    There was an outage affecting blah blah blah.
    The outage is reported here with red text.
    </p>
    </div>
    
    </div><!-- closing row -->

Make sure your message is correctly HTML formatted and all tags are closed
properly!  Even if your web browser is happy to display broken HTML, someone
else's web browser might not, which can potentially break the whole status
page.  Test formatting with a couple of web browsers (Chrome, Firefox, IE).

