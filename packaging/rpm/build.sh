#!/bin/sh
# packaging/rpm/build.sh — build an .rpm from a pre-compiled static binary.
#
# Uses rpmbuild's built-in topdir override so we don't pollute ~/rpmbuild.
#
# Usage:  build.sh <version> <rpm-arch> <binary-path> <output-dir>
#   rpm-arch: x86_64 | aarch64
set -eu

VERSION=$1
RPM_ARCH=$2
BINARY=$3
OUTDIR=$4

[ -x "$BINARY" ] || { echo "build.sh: binary not found: $BINARY" >&2; exit 1; }
command -v rpmbuild >/dev/null || { echo "build.sh: rpmbuild not installed" >&2; exit 1; }

here=$(cd "$(dirname "$0")" && pwd)
top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT

mkdir -p "$top/SOURCES" "$top/SPECS" "$top/BUILD" "$top/RPMS" "$top/SRPMS"

cp "$BINARY" "$top/SOURCES/agentty-linux-$RPM_ARCH"

sed -e "s/@VERSION@/$VERSION/g" \
    -e "s/@RPM_ARCH@/$RPM_ARCH/g" \
    -e "s/@DATE@/$(LC_ALL=C date '+%a %b %d %Y')/g" \
    "$here/agentty.spec.in" > "$top/SPECS/agentty.spec"

rpmbuild --define "_topdir $top" \
         --target "$RPM_ARCH" \
         -bb "$top/SPECS/agentty.spec" >/dev/null

mkdir -p "$OUTDIR"
out=$(find "$top/RPMS" -name '*.rpm' | head -1)
[ -n "$out" ] || { echo "build.sh: rpmbuild produced no rpm" >&2; exit 1; }
cp "$out" "$OUTDIR/"
final="$OUTDIR/$(basename "$out")"

printf '\033[1;32m✓\033[0m %s (%s)\n' "$final" "$(du -h "$final" | awk '{print $1}')"
