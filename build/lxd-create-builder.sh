#!/bin/sh
#
# Create an LXD container for building aprsc packages with build-all.pl.
# Works with images that configure themselves with cloud-init on first
# boot: Ubuntu cloud images from the ubuntu: remote, and the "cloud"
# variant of Fedora (imported with lxd-import-image.sh).
#
# See build/README.md for the whole procedure.
#

set -e

usage()
{
	cat <<EOF
Usage: $0 [options] <container-name> <image>

  e.g. $0 -a 192.0.2.10/24 -g 192.0.2.1 -n 192.0.2.53 -s example.com \\
          build-ubuntu-2604-amd64 ubuntu:26.04
       $0 -a 192.0.2.11/24 ... build-fedora-44-amd64 fedora-44-cloud

Options (defaults from the environment variable in brackets):
  -a ADDR/PREFIX   static IPv4 address of the container (required)
  -g GATEWAY       default gateway                  [BUILDER_GATEWAY]
  -n DNS[,DNS...]  name servers                     [BUILDER_NAMESERVERS]
  -s DOMAIN        DNS search domain                [BUILDER_SEARCH]
  -k FILE          SSH public key file to authorize, can be repeated;
                   default is the keys in ssh-agent (ssh-add -L)
  -u USER          build user created in the container [\$USER]
  -p PROFILE       LXD profile                      [default]
EOF
	exit 1
}

ADDRESS=""
GATEWAY="$BUILDER_GATEWAY"
NAMESERVERS="$BUILDER_NAMESERVERS"
SEARCH="$BUILDER_SEARCH"
KEYFILES=""
BUILDUSER="$USER"
PROFILE="default"

while getopts "a:g:n:s:k:u:p:h" opt; do
	case "$opt" in
		a) ADDRESS="$OPTARG" ;;
		g) GATEWAY="$OPTARG" ;;
		n) NAMESERVERS="$OPTARG" ;;
		s) SEARCH="$OPTARG" ;;
		k) KEYFILES="$KEYFILES $OPTARG" ;;
		u) BUILDUSER="$OPTARG" ;;
		p) PROFILE="$OPTARG" ;;
		*) usage ;;
	esac
done
shift $((OPTIND - 1))

[ $# -eq 2 ] || usage
NAME="$1"
IMAGE="$2"

[ -n "$ADDRESS" ] || { echo "$0: -a ADDR/PREFIX is required" >&2; exit 1; }
[ -n "$GATEWAY" ] || { echo "$0: -g GATEWAY is required" >&2; exit 1; }
[ -n "$NAMESERVERS" ] || { echo "$0: -n DNS is required" >&2; exit 1; }
[ -n "$BUILDUSER" ] || { echo "$0: -u USER is required" >&2; exit 1; }
case "$ADDRESS" in
	*/*) ;;
	*) echo "$0: address must have a prefix length, e.g. /24" >&2; exit 1 ;;
esac

if lxc info "$NAME" >/dev/null 2>&1; then
	echo "$0: container $NAME already exists" >&2
	exit 1
fi

SRCDIR=$(cd "$(dirname "$0")/.." && pwd)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ -n "$KEYFILES" ]; then
	cat $KEYFILES > "$TMP/keys"
else
	ssh-add -L > "$TMP/keys" || { echo "$0: no keys in ssh-agent, use -k" >&2; exit 1; }
fi
grep -q '^ssh-\|^ecdsa-' "$TMP/keys" || { echo "$0: no SSH public keys found" >&2; exit 1; }

echo "=== Creating $NAME from $IMAGE"
lxc init "$IMAGE" "$NAME" -p "$PROFILE" \
	-c security.nesting=true \
	-c security.syscalls.intercept.mknod=true

# Packages needed to build the .deb with "make make-deb" or the .rpm with
# "rpmbuild -ta"; the Build-Depends check at the end catches the Debian
# list going out of sync with src/debian/control. The .spec file only
# lists systemd, so the RPM list is checked by trying a build.
OS=$(lxc config get "$NAME" image.os | tr 'A-Z' 'a-z')
case "$OS" in
	ubuntu|debian)
		PACKAGES="build-essential debhelper fakeroot lsb-release lsb-base
			libevent-dev libssl-dev libcap-dev zlib1g-dev libsctp-dev
			libprotobuf-c-dev protobuf-c-compiler pkgconf openssh-server"
		SSHD=ssh
		;;
	fedora)
		PACKAGES="rpm-build gcc make perl-interpreter tar gzip
			libevent-devel openssl-devel libcap-devel zlib-devel
			lksctp-tools-devel protobuf-c-devel protobuf-c-compiler
			systemd-rpm-macros openssh-server"
		SSHD=sshd
		;;
	*)
		echo "$0: unsupported image OS '$OS', deleting $NAME" >&2
		lxc delete "$NAME"
		exit 1
		;;
esac

# cloud-init user-data: build user, packages, hostname
{
	echo "#cloud-config"
	echo "hostname: $NAME"
	echo "users:"
	echo "  - default"
	echo "  - name: $BUILDUSER"
	echo "    shell: /bin/bash"
	echo "    groups: [users]"
	echo "    lock_passwd: true"
	echo "    ssh_authorized_keys:"
	grep '^ssh-\|^ecdsa-' "$TMP/keys" | sed 's/^/      - /'
	echo "package_update: true"
	echo "package_upgrade: true"
	echo "packages:"
	for p in $PACKAGES; do
		echo "  - $p"
	done
	echo "runcmd:"
	echo "  - [ systemctl, enable, --now, $SSHD ]"
} > "$TMP/user-data"

# cloud-init network-config: static address on eth0
{
	echo "version: 2"
	echo "ethernets:"
	echo "  eth0:"
	echo "    dhcp4: false"
	echo "    addresses: [ $ADDRESS ]"
	echo "    routes:"
	echo "      - to: default"
	echo "        via: $GATEWAY"
	echo "    nameservers:"
	echo "      addresses: [ $(echo "$NAMESERVERS" | sed 's/,/, /g') ]"
	[ -n "$SEARCH" ] && echo "      search: [ $SEARCH ]"
} > "$TMP/network-config"

lxc config set "$NAME" user.user-data "$(cat "$TMP/user-data")"
lxc config set "$NAME" user.network-config "$(cat "$TMP/network-config")"

echo "=== Starting $NAME, waiting for cloud-init to install packages"
lxc start "$NAME"
# The container agent may not be ready for exec immediately
tries=0
until lxc exec "$NAME" -- true 2>/dev/null; do
	tries=$((tries + 1))
	[ $tries -lt 30 ] || { echo "$0: cannot exec in $NAME" >&2; exit 1; }
	sleep 1
done
# cloud-init status exits with 2 when it finished with warnings only.
# Fedora always warns "Failed to set the hostname", as hostnamectl has no
# D-Bus to talk to in the container; LXD has set the hostname already.
rc=0
lxc exec "$NAME" -- cloud-init status --wait || rc=$?
if [ $rc -ne 0 ]; then
	echo "$0: cloud-init finished with status $rc, see:" >&2
	echo "  lxc exec $NAME -- cloud-init status --long" >&2
	echo "  lxc exec $NAME -- cat /var/log/cloud-init-output.log" >&2
	[ $rc -eq 2 ] || exit 1
fi

if [ "$SSHD" = "ssh" ]; then
	echo "=== Checking aprsc Build-Depends"
	lxc file push "$SRCDIR/src/debian/control" "$NAME/tmp/aprsc-control"
	lxc exec "$NAME" -- dpkg-checkbuilddeps /tmp/aprsc-control
	lxc exec "$NAME" -- rm -f /tmp/aprsc-control
fi

echo "=== $NAME is ready"
echo "Distribution: $(lxc exec "$NAME" -- sh -c '. /etc/os-release; echo "$PRETTY_NAME${VERSION_CODENAME:+ ($VERSION_CODENAME)}"')"
echo "SSH host key fingerprints, to compare on the first ssh login:"
lxc exec "$NAME" -- sh -c 'for f in /etc/ssh/ssh_host_*_key.pub; do ssh-keygen -lf "$f"; done'
