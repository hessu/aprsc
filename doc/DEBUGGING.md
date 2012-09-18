
# allow binary doing setuid() to dump core
echo 2 > /proc/sys/fs/suid_dumpable

echo "/var/core/core.%e.%p.%s" > /proc/sys/kernel/core_pattern 

mkdir -p /opt/aprsc/var/core
chmod a+w /opt/aprsc/var/core

