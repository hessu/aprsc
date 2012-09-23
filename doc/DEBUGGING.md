
Debugging aprsc issues, tips and tricks
=======================================

Enabling debug logging
-------------------------

Change the log level to debug by changing "-e info" to "-e debug" on the
aprsc command line.  Insert "-e debug" if you don't have an -e parameter.

On Debian and Ubuntu the parameter goes in /etc/default/aprsc, on the
DAEMON_OPTS line. On other systems it typically goes in the init script.

Enabling core dumps on Linux
-------------------------------

Allow binary doing setuid() to dump core:

    echo 2 > /proc/sys/fs/suid_dumpable

Tune core file name pattern:

    echo "/var/core/core.%e.%p.%s" > /proc/sys/kernel/core_pattern 

Create a directory within the aprsc chroot where the core can be
dumped:

    mkdir -p /opt/aprsc/var/core
    chmod a+w /opt/aprsc/var/core

