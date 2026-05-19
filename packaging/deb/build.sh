#!/bin/sh
# packaging/deb/build.sh — produce a .deb without any Debian tooling.
#
# Uses `ar` + `tar` only (both POSIX-standard, present on Arch/Alpine/macOS).
# This is exactly the format dpkg-deb emits — `ar` archive of three members:
#   debian-binary  (literal "2.0\n")
#   control.tar.gz (DEBIAN/control + md5sums)
#   data.tar.gz    (filesystem tree relative to /)
#
# Usage:  build.sh <version> <deb-arch> <binary-path> <output-dir>
#   deb-arch: amd64 | arm64 | armhf | i386 …
set -eu

VERSION=$1
ARCH=$2
BINARY=$3
OUTDIR=$4

[ -x "$BINARY" ] || { echo "build.sh: binary not found or not executable: $BINARY" >&2; exit 1; }

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# --- staged filesystem tree ---------------------------------------------------
mkdir -p "$work/data/usr/bin"
mkdir -p "$work/data/usr/share/doc/agentty"
install -Dm755 "$BINARY"                       "$work/data/usr/bin/agentty"
install -Dm644 "$here/../../LICENSE"           "$work/data/usr/share/doc/agentty/copyright" 2>/dev/null || true
install -Dm644 "$here/../../README.md"         "$work/data/usr/share/doc/agentty/README.md"  2>/dev/null || true

# --- DEBIAN/control + md5sums -------------------------------------------------
mkdir -p "$work/control"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@ARCH@/$ARCH/g" \
    "$here/control.in" > "$work/control/control"

# Installed-Size in KiB (du -k rounds up)
size_kib=$(du -ks "$work/data" | awk '{print $1}')
printf 'Installed-Size: %s\n' "$size_kib" >> "$work/control/control"

# md5sums for every file under data/, paths relative to /
( cd "$work/data" && find . -type f -print0 \
    | xargs -0 md5sum \
    | sed 's| \./| |' ) > "$work/control/md5sums"

# --- assemble archives --------------------------------------------------------
( cd "$work/control" && tar --owner=0 --group=0 --mtime='@0' \
    -czf "$work/control.tar.gz" . )
( cd "$work/data"    && tar --owner=0 --group=0 --mtime='@0' \
    -czf "$work/data.tar.gz" . )

printf '2.0\n' > "$work/debian-binary"

mkdir -p "$OUTDIR"
out="$OUTDIR/agentty_${VERSION}_${ARCH}.deb"
( cd "$work" && ar rcs "$out" debian-binary control.tar.gz data.tar.gz )

printf '\033[1;32m✓\033[0m %s (%s)\n' "$out" "$(du -h "$out" | awk '{print $1}')"
