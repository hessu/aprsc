
Running multiple copies of aprsc
===================================

The Debian packages of aprsc (starting with version 2.0.11) support running
multiple instances of aprsc on the same host.  Using this method all of the
instances will run the same version of aprsc, and they can be started,
stopped and upgraded using a single command.

Each instance requires its own unique data directory (configured with RunDir).

Instances are named with a suffix. Let's say you want to run a second server
named **FOO** in addition to the regular one.  We shall pick `foo` as the
suffix, and the full name of the instance will then be `aprsc-foo`.  My CWOP
server is set up with the instance name `aprsc-cwop`.

Log and config files are named by the full instance name, i.e. 
`/opt/aprsc/etc/aprsc-foo.conf`, `/opt/aprsc/logs/aprsc-foo.log`, and so on. 
Not surprisingly, the "normal" default instance is simply named `aprsc`
without a -suffix.

Due to shell variable name restrictions the instance suffix may not have
special characters, or the '-' character which happens to be special enough. 
Go with an alphanumeric suffix such as 'foo' or 'cwop3' to stay on the safe
side.


Configuration file and RunDir
--------------------------------

Create a configuration file for the instance, name it
`/opt/aprsc/etc/aprsc-foo.conf`.  The configuration file name must match the
instance name.

* On the `foo` instance, set RunDir to `data/foo`. Each instance must
  have a separate RunDir - otherwise live upgrade will likely fail
  miserably.  Due to the chroot setup RunDir must be a relative path,
  i.e. `data/foo` - try to resist the urge of adding the `/opt/aprsc`
  prefix to it.
* Create the data directory:
  `sudo mkdir /opt/aprsc/data/foo`
* Change its ownership so that aprsc can write there:
  `sudo chown aprsc:aprsc /opt/aprsc/data/foo`
* Make sure all of the instances either **listen on different ports or
  different IP addresses**.  If one instance listens on a wildcard address
  (0.0.0.0:14580), another instance can not listen on a specific address
  on the same port (192.168.1.2:14580) - all servers need to bind
  specific addresses in this case.
* The listening rules apply to HTTP services, too.


Defaults file and custom command line options
------------------------------------------------

Edit `/etc/default/aprsc` and add a new parameter **DAEMON_OPTS_BASE** in
the end.  The parameter sets default command line options to all the
additional instances, including our new `aprsc-foo`.  **DAEMON_OPTS_BASE**
should have the same common options as **DAEMON_OPTS** - just remove the
instance-specific options (-c and -n).  Here's what I use:

    DAEMON_OPTS_BASE="-u aprsc -t /opt/aprsc -f -e info -o file -r logs"

The init script will automatically add `-c etc/aprsc-foo.conf` on the
command line so that aprsc will find the correct configuration file, and `-n
aprsc-foo` so that the instance will have its own log and pid files in
`/opt/aprsc/logs`.

If you wish to add instance-specific options **other than -n or -c**, you
can set them in **DAEMON_OPTS_foo** (or **DAEMON_OPTS_instancesuffix**).


Starting up and shutting down
--------------------------------

By default the aprsc init script starts and stops all instances when you run
`sudo service aprsc start` or `sudo service aprsc stop` (or any of the other
options such as `reload` or `restart`).  You can specify an instance to
start or stop by adding the instance name in the end: `sudo service aprsc
start aprsc-foo`.

If you wish to only start up specific instances automatically when the
system boots up, list the instance names in `/etc/default/aprsc` parameter
**AUTOSTART**:

    AUTOSTART="aprsc-foo aprsc-cwop"


Upgrading
------------

When you install a new version of aprsc using dpkg or apt-get, all instances
should be automatically upgraded live.

