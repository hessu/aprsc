
Installing aprsc on Windows
===========================

This is a beta-level version of aprsc for Windows.  It appears to mostly
work, but it's likely that something "interesting" will pop up.

This build has been done using [Cygwin][cygwin], a collection of tools
which enables you to compile and run Linux and Unix applications on Windows.

The aprsc for Windows package contains all the necessary Cygwin bits needed
to run aprsc, so you don't need to install Cygwin first.

Only one exact version of Cygwin must be used on a single Windows box at a
time.  If you do install cygwin for some reason, you need to manually update
the Cygwin files in the aprsc directory with copies from your own cygwin
installation to make sure both your cygwin and aprsc run the same version.

aprsc for Windows requires an i686 (Pentium 2 class) CPU, or anything newer
than that.  It has been (briefly) tested on Windows XP, Windows 7 and
Windows 8.

[cygwin]: http://www.cygwin.com/


Download aprsc
-----------------

Grab the latest version from the [Windows installers download
directory][downloads].

[downloads]: http://he.fi/aprsc/down/win/


Extract it
-------------

Extract the contents of the zip file to C:\aprsc\.  You should end up with
a small set of .cmd files to start up and shutdown aprsc, a couple of .exe
files and a number of .dll files in C:\aprsc\, and a few subdirectories.
\aprsc\web\ contains the status display web content served by aprsc's
internal HTTP server, \aprsc\logs\ will contain log files, \aprsc\data\ is
empty at first but might contain aprsc's data files at some point.
\aprsc\doc\ contains copies of aprsc documentation.


Configure it
---------------

Before starting aprsc edit the configuration file, which can be found in
c:\aprsc\aprsc.conf.  Please see the [CONFIGURATION](CONFIGURATION.html)
document for instructions.


Run it
---------

aprsc runs as a Windows service. When the service is set up, it will also
start up automatically at boot. The windows installer .zip file comes with
five CMD files to manage the service:


*   aprsc-enable.cmd

    Configures aprsc as a Windows service. Does not start it up, but makes
    it start up after a reboot.  Run this first!

*   aprsc-start.cmd

    Starts up the aprsc service after it has been set up using aprsc-enable.
    Won't do anything if aprsc-enable.cmd has not been executed first.

*   aprsc-stop.cmd

    Stops the aprsc service. It'll start up again on the next reboot,
    though.

*   aprsc-disable.cmd

    Removes the aprsc service configuration from Service Manager.

*   aprsc-status.cmd

    Queries the status of the aprsc service from Service Manager.


As usual, you don't need to type the .cmd file extension:

    C:\aprsc> aprsc-enable
    C:\aprsc> aprsc-start


After running aprsc-enable, you can also control the service using the Windows
Services tool.

