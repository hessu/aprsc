#! /bin/sh

# copy files required for chrooted operation, use bind mounts to expose
# libraries

BASEDIR=/opt/aprsc
DIRNAME=aprsc

prepare_chroot () {
	# config files
	/bin/cp -p /etc/resolv.conf /etc/nsswitch.conf /etc/hosts /etc/gai.conf $BASEDIR/etc/
	# live upgrade requires libraries to be visible within chroot, so
	# set up a read-only bind mount of /lib
	grep -q "$DIRNAME/lib " /proc/mounts || \
		( mount --bind /lib $BASEDIR/lib \
		&& mount -o remount,ro,bind $BASEDIR/lib )
	if [ -e /lib64 ]; then
		grep -q "$DIRNAME/lib64 " /proc/mounts || \
			( mount --bind /lib64 $BASEDIR/lib64 \
			&& mount -o remount,ro,bind $BASEDIR/lib64 )
	fi
	grep -q "$DIRNAME/usr/lib " /proc/mounts || \
		( mount --bind /usr/lib $BASEDIR/usr/lib \
		&& mount -o remount,ro,bind $BASEDIR/usr/lib )
	if [ -e /usr/lib64 ]; then
		grep -q "$DIRNAME/usr/lib64 " /proc/mounts || \
			( mount --bind /usr/lib64 $BASEDIR/usr/lib64 \
			&& mount -o remount,ro,bind $BASEDIR/usr/lib64 )
	fi
}

prepare_chroot

