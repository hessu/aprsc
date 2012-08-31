
Installing aprsc
================

aprsc is "officially" "supported" on the following platforms:

* Debian stable (6.0, "squeeze"): i386 and x86_64
* Ubuntu LTS (10.04, 12.04): i386 and x86_64

These platforms are the easiest to install, and upgrades happen
automatically using the mechanisms provided by the operating system.  One or
two other modern Linux distributions might become supported in the near
future, too.

If you're familiar with compiling software from the source code, and your
preferred operating system is NOT listed above, take a look at BUILDING.md
for documentation on building from source.

If you wish to have decent support, please pick Debian or Ubuntu.  A number
of other Unix-like platforms do work, but when it comes to building and
installing, you're mostly on your own.


Debian and Ubuntu: Installing using apt-get
----------------------------------------------

As the first step, please configure aprsc's package repository for apt. 
You'll need to figure out the codename of your distribution.  The command
"lsb_release -c" should provide the codename.  Here's a list of distribution
versions and their codenames:

* Debian 6.0: squeeze
* Ubuntu 12.04 LTS: precise
* Ubuntu 10.04 LTS: lucid

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

    gpg --keyserver keys.gnupg.net --recv 657A2B8D
    gpg --export C51AA22389B5B74C3896EF3CA72A581E657A2B8D | sudo apt-key add -

Next, download the package indexes:

    sudo apt-get update

Then, install aprsc:

    sudo apt-get install aprsc

Whenever a new aprsc version is available, the upgrade can be performed
automatically by running the upgrade command.  Your operating system can
also be configured to upgrade packages automatically, or instruct you to
upgrade when upgrades are available.

    sudo apt-get upgrade

Before starting aprsc edit the configuration file, which can be found in
/opt/aprsc/etc/aprsc.conf.  Please see the CONFIGURATION file for
instructions.

To enable startup, edit /etc/default/aprsc and change STARTAPRSC="no" to
"yes". There should not be any need to touch the other options at this time.

Start it up:

    sudo service aprsc start

To shut it down:

    sudo service aprsc stop

When STARTAPRSC is set to YES in the /etc/default/aprsc file it will
automatically start up when the system boots.  You'll find it's log file in
/opt/aprsc/logs/aprsc.log.  Log rotation is already configured in
aprsc.conf.

