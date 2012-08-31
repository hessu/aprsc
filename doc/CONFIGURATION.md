
Configuring aprsc
=================

Command line parameters and startup
--------------------------------------

aprsc understands a few command line parameters. On Ubuntu/Debian derived
systems these go to /etc/default/aprsc, on other systems they go in the init
script starting up the software.

 *  -u aprsc: switch to user 'aprsc' as soon as possible
 *  -t /opt/aprsc: chroot to the given directory after starting
 *  -f: fork to a daemon
 *  -e info: log at level info (can be changed to "debug" for more verbose
    logging)
 *  -o file: log to file (can be changed to "stderr" for supervisord and debugging, or "syslog"
   for syslogd)
 *  -r logs: log file directory, log files are placed in /opt/aprsc/logs
 *  -c etc/aprsc.onf: configuration file location

Since the daemon chroots to /opt/aprsc, all paths are relative to
that directory and the daemon cannot access any files outside
the chroot.

aprsc can log to syslog too, but that'd require bringing the
syslog socket within the chroot.


Configuration file options in aprsc.conf
-------------------------------------------

### Basic server options ###

 *  ServerId NOCALL
    
    This is your unique server ID. Typically contains either a callsign and
    an SSID.

 *  PassCode 0
    
    This is the passcode for your server ID.

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


### Timers and timeouts ###

Timer settings assume the time is specified in seconds, but allow appending
one of s, m, h, d to multiply the given value by the number of seconds in a
second, month, hour and day respectively. Examples:

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

    Listen socketname porttype tcp address to bind port options...


 *  socketname: any name you wish to show up in logs and statistics

 *  porttype: one of:
 
    - fullfeed - everything, after dupe filtering
    - igate - igate / client port with user-specified filters
    - udpsubmit - UDP packet submission port (8080)
    - dupefeed - duplicate packets dropped by the server

 *  options: one more of:
 
    - filter "m/500 t/m" - force a filter for users connected here.
      If you wish to specify multiple filters, put them in the same "string",
      separated by spaces.
    - maxclients 100 - limit clients connected on this port (defaults to 200)
    - acl etc/client.acl - match client addresses against ACL
    - hidden - don't show the port in the status page

#
#              If you wish to provide UDP service for clients, set up a
#              second listener on the same address, port and protocol.
#
#              The  "::"  is IPv6 "IN6ADDR_ANY", whereas "0.0.0.0" is same
#              with IPv4.
#
#              On FreeBSD you need to have separate listeners for IPv4 and
#              IPv6. On Linux, just use :: alone - the IPv6 listener will
#              catch the IPv4 connections just as well.
#
# Example of normal server ports for Linux, supporting both TCP and UDP,
# IPv4 and IPv6:
# 
Listen "Full feed"                                fullfeed tcp ::  10152 hidden
Listen ""                                         fullfeed udp ::  10152 hidden

Listen "Client-Defined Filters"                   igate tcp ::  14580
Listen ""                                         igate udp ::  14580

#Listen "350 km from my position"                 fullfeed tcp ::  20350 filter "m/350"
#Listen ""                                        fullfeed udp ::  20350 filter "m/350"

Listen "UDP submit"                               udpsubmit udp :: 8080



