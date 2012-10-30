
Monitoring aprsc
================

It's a good practice to monitor the performance and utilisation of your
server, and to generate alarms when it's not working as it should.

aprsc comes with a munin plugin which makes it very easy to set up
statistics graphs.  Comparing graphs for aprsc against the graphs for the
rest of the system can be very helpful in diagnosing performance issues. 
Seeing all those statistics also gives a nice warm fuzzy feeling from
knowing how how the server is doing and what it is spending all those
electrons on.

For example, please take a look at my [aprsc-specific Munin graphs from
T2FINLAND](http://he.fi/aprsc/munin/).


Setting up munin on Debian or Ubuntu
---------------------------------------

Munin consists of two pieces, an agent running on all servers (munin-node),
collecting raw numbers from the operating system and the software running on
it, and the master data collector (munin).  If you have many servers, put
the master data collector on just one of them (that server needs to have a
web server to publish the statistics).  If you have a single server, put a
web server, the agent and the master on that.

Installing the agent, the master and the standard Apache web server:

    sudo apt-get install munin-node munin apache2-mpm-worker

It will pull up quite a few other packages as dependencies, but that's OK.


Setting up munin on other Linux distributions
------------------------------------------------

Instructions for each distribution can be found [on munin's home
page](http://munin-monitoring.org/wiki/LinuxInstallation).


Setting up the munin plugin
------------------------------

If munin is installed when installing or upgrading aprsc, aprsc's
post-install script will automatically configure aprsc's munin plugin.

If you have aprsc running already, reconfigure it:

    sudo dpkg-reconfigure aprsc

That'll set up the munin plugin (among a few other things, like triggering a
live upgrade to the current version).  Now, you'll need to give munin a kick
to make it notice the new plugins:

    sudo /etc/init.d/munin-node restart

Future versions of aprsc might do that automatically for you.


Wait!
--------

Wait for some 10-15 minutes.  Have a nice cup of coffee or tea, or some
other beverage according to your personal preference.  Munin updates every 5
minutes, but after installation or adding new plugins, it'll take a couple
of rounds before it starts generating graphs for those.


See.
-------

Surf to http://yourserver.example.com/munin/ and browse around!
Ooops, got a "403 Forbidden" error? Proceed to the next step.


Edit the web server's config file
------------------------------------

At least on debian, the web server is configured by default to only allow a
local web browser to access the Munin subdirectory.  Open up
/etc/apache2/conf.d/munin in your favourite text editor, and put your own IP
address, network or domain on and allow line.  You can add new Allow lines
next to the one that's already there, like this:

    Allow from 44.0.0.0/8
    Allow from .ampr.org

After editing apache's config file, tell it to re-read configuration by
doing a graceful restart:

    sudo apache2ctl graceful

And surf to http://yourserver.example.com/munin/ again.


Setting up nagios alarms
---------------------------

TODO: write
