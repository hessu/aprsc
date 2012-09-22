
Debugging aprsc issues, tips and tricks
=======================================


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

