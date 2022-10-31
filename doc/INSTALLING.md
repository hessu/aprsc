
Installing aprsc
================

aprsc is "officially" "supported" on the following platforms:

* Debian stable (11.0, "bullseye"): i386, x86_64
* Debian oldstable (10.0, "buster"): i386, x86_64
* Debian oldoldstable (9.0, "stretch"): i386, x86_64
* Ubuntu LTS (18.04, 20.04, 22.04): x86_64
* CentOS 7: x86_64

The i386 builds actually require an i686 (Pentium 2 class) CPU or
anything newer than that.

These platforms are the easiest to install, and upgrades happen
automatically using the mechanisms provided by the operating system.  The list of
supported Linux distributions is not likely to become longer, since it takes
a noticeable amount of time to support each distribution.

If you're familiar with compiling software from the source code, and your
preferred operating system is NOT listed above, take a look at
[BUILDING](BUILDING.html) for documentation on building from source.

If you wish to have decent support, please pick Debian or Ubuntu.  A number
of other Unix-like platforms do work, but when it comes to building and
installing, you're mostly on your own.


Call-home functionality
--------------------------

When aprsc starts up, it makes a DNS lookup to SERVERID.VERSION.aprsc.he.fi
for the purpose of preloading DNS resolver libraries before chroot, so that
DNS works even after the chroot.  The lookup also serves as a call-home
functionality, which provides the aprsc developers the possibility of collecting
a database of different aprsc installations and software versions being used
at those servers.

Statistics graphs showing the aggregate number of aprsc servers running and
the aprsc versions use will be shown on the aprsc home page once the data
collector (custom DNS server) and graphing scripts have been written.


Debian and Ubuntu: Installing using apt-get
----------------------------------------------

As the first step, please configure aprsc's package repository for apt. 
You'll need to figure out the codename of your distribution.  The command
"lsb_release -c" should provide the codename.  Here's a list of distribution
versions and their codenames:

* Ubuntu 22.04 LTS: jammy
* Ubuntu 20.04 LTS: focal
* Ubuntu 18.04 LTS: bionic
* Debian 11.0: bullseye
* Debian 10.0: buster
* Debian 9.0: stretch

Other versions are currently not supported.

Next, add the following line in the end of your /etc/apt/sources.list file:

    deb http://aprsc-dist.he.fi/aprsc/apt DISTRIBUTION main

Naturally, DISTRIBUTION needs to be replaced with your distributions
codename (squeeze, or whatever).  You should see the codename appearing on
other similar "deb" lines in sources.list.

Editing sources.list requires root privileges.  The following commands assume
you're running them as a regular user, and the sudo tool is used to run
individual commands as root.  sudo will ask you for your password.

Next, add the gpg key used to sign the packages by running the following
commands at your command prompt.  This will enable strong authentication of
the aprsc packages - apt-get will cryptographically validate them.

    gpg --keyserver keyserver.ubuntu.com --recv C51AA22389B5B74C3896EF3CA72A581E657A2B8D
    gpg --export C51AA22389B5B74C3896EF3CA72A581E657A2B8D | sudo apt-key add -

Next, download the package indexes:

    sudo apt-get update

Then, install aprsc:

    sudo apt-get install aprsc

Whenever a new aprsc version is available, an upgrade can be performed
automatically by running the upgrade command.  Your operating system can
also be configured to upgrade packages automatically, or to instruct you to
upgrade when upgrades are available. The following upgrade command will also
restart aprsc for you, if possible.

    sudo apt-get update && sudo apt-get upgrade

Before starting aprsc edit the configuration file, which can be found in
/opt/aprsc/etc/aprsc.conf.  Please see the [CONFIGURATION](CONFIGURATION.html)
document for instructions.

If your distribution has systemd (try 'systemctl' to find out if you do),
proceed with the systemd instructions.  If not, proceed with the
old-fashioned non-systemd instructions.

Startup with systemd
-----------------------

Enable the service:

    sudo systemctl enable aprsc

To start it up:

    sudo systemctl start aprsc

To shut it down:

    sudo systemctl stop aprsc

To perform a restart:

    sudo systemctl restart aprsc


Startup the old-fashioned way
--------------------------------

To enable startup, edit /etc/default/aprsc and change STARTAPRSC="no" to
"yes". There should not be any need to touch the other options at this time.

Start it up:

    sudo service aprsc start

To shut it down:

    sudo service aprsc stop

To perform a restart:

    sudo service aprsc restart

When STARTAPRSC is set to YES in the /etc/default/aprsc file it will
automatically start up when the system boots.  You'll find it's log file in
/opt/aprsc/logs/aprsc.log.  Log rotation is already configured in
aprsc.conf.

After startup, look at the log file for startup messages, watch out for
any warnings or errors.


CentOS: Installing using yum
-------------------------------

This installation procedure has only been tested on CentOS 6.8 and 7.0. It
should probably work from 6.0 to 6.8 on both i386 and x86\_64 platforms. 7.0
builds are only available for x86\_64 currently.

The following commands assume you're running them as a regular user, and the
sudo tool is used to run individual commands as root.  sudo will ask you for
your password.

As the first step, please configure aprsc's package repository in yum by
downloading the .repo configuration file and installing it.  The first
command installs curl (if you don't have it already), and the second command
uses curl to download the repository configuration to the right place.

    sudo yum install curl
    sudo curl -o /etc/yum.repos.d/aprsc.repo http://he.fi/aprsc/down/aprsc-centos.repo

Then, install aprsc:

    sudo yum install aprsc

Whenever a new aprsc version is available, the upgrade can be performed
automatically by running the upgrade command.  Your operating system can
also be configured to upgrade packages automatically, or instruct you to
upgrade when upgrades are available.

    sudo yum upgrade

If aprsc upgrades happen very often (many times per day), you might have to
tell yum to expire it's cache before executing the upgrade command:

    sudo yum clean expire-cache

Before starting aprsc edit the configuration file, which can be found in
/opt/aprsc/etc/aprsc.conf.  Please see the [CONFIGURATION](CONFIGURATION.html)
document for instructions.

To enable startup, edit /etc/sysconfig/aprsc and change STARTAPRSC="no" to
"yes". There should not be any need to touch the other options at this time.

Start it up:

    sudo /etc/init.d/aprsc start

To shut it down:

    sudo /etc/init.d/aprsc stop

To perform a restart after upgrading aprsc:

    sudo /etc/init.d/aprsc restart

When STARTAPRSC is set to YES in the /etc/sysconfig/aprsc file it will
automatically start up when the system boots.  You'll find it's log file in
/opt/aprsc/logs/aprsc.log.  Log rotation is already configured in
aprsc.conf.

After startup, look at the log file for startup messages, watch out for
any warnings or errors.


