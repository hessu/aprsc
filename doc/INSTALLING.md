
Installing aprsc
================

aprsc is "officially" "supported" on the following platforms:

* Debian stable (12.0, "bookworm"): x86_64
* Debian oldstable (11.0, "bullseye"): i386, x86_64
* Debian oldoldstable (10.0, "buster"): i386, x86_64
* Ubuntu LTS (20.04, 22.04, 24.04): x86_64
* Fedora Core 39 and 40: x86_64

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

* Ubuntu 24.04 LTS: noble
* Ubuntu 22.04 LTS: jammy
* Ubuntu 20.04 LTS: focal
* Debian 12.0: bookworm
* Debian 11.0: bullseye
* Debian 10.0: buster

Other versions are currently not supported.

Next, create a file called `/etc/apt/sources.list.d/aprsc.list` using your
favourite text editor, and add the following line in it:

    deb http://aprsc-dist.he.fi/aprsc/apt DISTRIBUTION main

Naturally, DISTRIBUTION needs to be replaced with your distributions
codename (bookworm, or whatever).  You should see the codename appearing on
other similar "deb" lines in /etc/apt/sources.list.  It is possible to add
the `deb` line in `sources.list` instead of `aprsc.list`, but having a
separate file for additional repositories is preferable.

Editing sources.list.d files requires root privileges.  The following
commands assume you're running them as a regular user, and the sudo tool is
used to run individual commands as root.  sudo will ask you for your
password.

Next, add the gpg key used to sign the packages by running the following
commands at your command prompt.  This will enable strong authentication of
the aprsc packages - apt-get will cryptographically validate them.

On newer distributions, such as Debian 12.0 and Ubuntu 22.04, where the
`/etc/apt/trusted.gpg.d` directory is present, run the following commands:

    gpg --keyserver keyserver.ubuntu.com --recv C51AA22389B5B74C3896EF3CA72A581E657A2B8D
    sudo gpg --export C51AA22389B5B74C3896EF3CA72A581E657A2B8D > /etc/apt/trusted.gpg.d/aprsc.key.gpg

If you get a warning saying `Key is stored in legacy trusted.gpg keyring`,
it can be deleted from there like this:

    sudo apt-key del C51AA22389B5B74C3896EF3CA72A581E657A2B8D

On older distributions where `trusted.gpg.d` directory is not present, use
the following commands instead:

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


You'll find it's log file in /opt/aprsc/logs/aprsc.log.  Log rotation is
already configured in aprsc.conf.

After startup, look at the log file for startup messages, watch out for
any warnings or errors.


Fedora Core: Installing using dnf
------------------------------------

This installation procedure has only been tested on Fedora 39 and 40.
Builds are only available for x86\_64 currently.

The following commands assume you're running them as a regular user, and the
sudo tool is used to run individual commands as root.  sudo will ask you for
your password.

As the first step, please configure aprsc's package repository in dnf by
downloading the .repo configuration file and installing it.  The first
command installs curl (if you don't have it already), and the second command
uses curl to download the repository configuration to the right place.

    sudo dnf install curl
    sudo curl -o /etc/yum.repos.d/aprsc.repo http://he.fi/aprsc/down/aprsc-fedora.repo

Then, install aprsc:

    sudo dnf install aprsc

Whenever a new aprsc version is available, the upgrade can be performed
automatically by running the upgrade command.  Your operating system can
also be configured to upgrade packages automatically, or instruct you to
upgrade when upgrades are available.

    sudo dnf upgrade

If aprsc upgrades happen very often (many times per day), you might have to
tell dnf to expire it's cache before executing the upgrade command:

    sudo dnf clean expire-cache

Before starting aprsc edit the configuration file, which can be found in
/opt/aprsc/etc/aprsc.conf.  Please see the [CONFIGURATION](CONFIGURATION.html)
document for instructions.

Then proceed with the "Startup with systemd" instructions above.

