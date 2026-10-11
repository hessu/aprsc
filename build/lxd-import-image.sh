#!/bin/sh
#
# Import a container image from images.linuxcontainers.org into the local
# LXD image store. The server only serves its image index to Incus, but
# the image files can be downloaded and imported into LXD, which reads
# the same metadata format.
#
# See build/README.md.
#

set -e

usage()
{
	cat <<EOF
Usage: $0 <distribution> <release> [variant] [architecture]

  e.g. $0 fedora 44
       imports the latest fedora/44/amd64/cloud image as alias fedora-44-cloud

  variant defaults to cloud (has cloud-init), architecture to amd64.
EOF
	exit 1
}

[ $# -ge 2 ] && [ $# -le 4 ] || usage
DIST="$1"
RELEASE="$2"
VARIANT="${3:-cloud}"
ARCH="${4:-amd64}"

BASE="https://images.linuxcontainers.org/images/$DIST/$RELEASE/$ARCH/$VARIANT"
ALIAS="$DIST-$RELEASE-$VARIANT"
[ "$ARCH" = "amd64" ] || ALIAS="$ALIAS-$ARCH"

if lxc image info "$ALIAS" >/dev/null 2>&1; then
	echo "$0: image alias $ALIAS already exists, remove it first with:" >&2
	echo "  lxc image delete $ALIAS" >&2
	exit 1
fi

# Build directories are named by date, e.g. 20261011_20:33, so the last
# one in sort order is the newest.
SERIAL=$(curl -sfL "$BASE/" | sed -n 's/.*href="\([0-9][0-9_%A-F]*\)\/".*/\1/p' | sort | tail -1)
[ -n "$SERIAL" ] || { echo "$0: no images found at $BASE/" >&2; exit 1; }
URL="$BASE/$SERIAL"
echo "=== Downloading $URL"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for f in SHA256SUMS incus.tar.xz rootfs.squashfs; do
	curl -sfL -o "$TMP/$f" "$URL/$f" || { echo "$0: failed to download $f" >&2; exit 1; }
done

(cd "$TMP" && grep -E ' (incus\.tar\.xz|rootfs\.squashfs)$' SHA256SUMS | sha256sum -c)

echo "=== Importing as $ALIAS"
lxc image import "$TMP/incus.tar.xz" "$TMP/rootfs.squashfs" --alias "$ALIAS"
lxc image info "$ALIAS" | grep -E '^(Fingerprint|Architecture)|description:'
