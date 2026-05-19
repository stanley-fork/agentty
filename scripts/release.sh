#!/bin/sh
# scripts/release.sh — one-shot reproducible release builder.
#
# Builds every artifact for a new tagged release from this Linux box,
# without any CI:
#
#   dist/agentty-linux-x86_64                     fully-static musl
#   dist/agentty-linux-aarch64                    fully-static musl
#   dist/agentty-windows-x86_64.exe               existing release asset (kept)
#   dist/agentty_<ver>_amd64.deb                  .deb (debian/ubuntu)
#   dist/agentty_<ver>_arm64.deb                  .deb
#   dist/agentty-<ver>-1.x86_64.rpm               .rpm (fedora/rhel)
#   dist/agentty-<ver>-1.aarch64.rpm              .rpm
#   dist/agentty-<ver>-1-x86_64.pkg.tar.zst       arch .pkg (binary install)
#   dist/agentty-<ver>-1-aarch64.pkg.tar.zst      arch .pkg
#   dist/agentty-<ver>.tar.gz / .tar.xz           source tarball (for homebrew)
#   dist/SHA256SUMS                               every asset
#   dist/packaging/agentty.rb                     homebrew formula w/ real sha256s
#   dist/packaging/agentty.json                   scoop manifest w/ real sha256
#   dist/packaging/PKGBUILD                       AUR PKGBUILD w/ real sha256s
#
# Usage:
#   scripts/release.sh                  # build everything, no upload
#   scripts/release.sh --tag v0.1.1     # build everything, tag, upload via gh
#   scripts/release.sh --skip-binaries  # reuse existing dist/agentty-linux-*
#                                       # (handy when iterating on packaging)
#
# Requirements (auto-detected, missing steps SKIPPED with a yellow note):
#   docker      static musl builds for linux-{x86_64,aarch64}
#   ar + tar    .deb (always present)
#   rpmbuild    .rpm
#   makepkg     arch .pkg.tar.zst
#   gh          release upload (only when --tag is passed)
#
# Idempotent: re-runnable. Existing artifacts are overwritten; nothing
# outside dist/ is touched. No system packages installed.

set -eu

# ---- args --------------------------------------------------------------------
TAG=""
SKIP_BINARIES=0
UPLOAD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --tag)            TAG=$2; UPLOAD=1; shift 2 ;;
        --skip-binaries)  SKIP_BINARIES=1; shift ;;
        --no-upload)      UPLOAD=0; shift ;;
        -h|--help)        sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "release.sh: unknown arg: $1" >&2; exit 2 ;;
    esac
done

# ---- paths -------------------------------------------------------------------
root=$(cd "$(dirname "$0")/.." && pwd)
dist="$root/dist"
pkgdir="$dist/packaging"
mkdir -p "$dist" "$pkgdir"

# ---- version (single source of truth: CMakeLists.txt) -----------------------
# `project(agentty VERSION X.Y.Z LANGUAGES CXX)` — every artifact, manifest,
# and tag is derived from this one line.
VERSION=$(sed -nE 's/.*project\(agentty VERSION ([0-9.]+).*/\1/p' "$root/CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "release.sh: could not read VERSION from CMakeLists.txt" >&2; exit 1; }
if [ -n "$TAG" ]; then
    expected="v$VERSION"
    [ "$TAG" = "$expected" ] || {
        printf '\033[1;33m!\033[0m tag %s does not match CMakeLists VERSION %s — bump CMakeLists first\n' "$TAG" "$VERSION" >&2
        exit 1
    }
fi

# ---- ui ----------------------------------------------------------------------
hr()   { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!\033[0m  %s\n' "$*"; }
ok()   { printf '\033[1;32m\xe2\x9c\x93\033[0m %s\n' "$*"; }
skip() { printf '\033[1;33mskip\033[0m %s\n' "$*"; }

hr "agentty release builder — version $VERSION"

# ---- 1. linux static binaries via docker -------------------------------------
hr "1/6  linux static binaries"
if [ "$SKIP_BINARIES" -eq 1 ]; then
    skip "binaries (--skip-binaries)"
    for a in x86_64 aarch64; do
        [ -x "$dist/agentty-linux-$a" ] || {
            warn "missing $dist/agentty-linux-$a — run without --skip-binaries"
            exit 1
        }
    done
elif command -v docker >/dev/null 2>&1; then
    for arch_pair in 'x86_64 amd64' 'aarch64 arm64'; do
        # shellcheck disable=SC2086
        set -- $arch_pair
        arch=$1; docker_arch=$2
        info "building agentty-linux-$arch (alpine + musl, $docker_arch)"
        docker run --rm \
            --platform "linux/$docker_arch" \
            -v "$root":/src -w /src \
            alpine:3.21 sh -c '
                apk add --no-cache build-base cmake ninja git openssl-dev openssl-libs-static \
                                   nghttp2-dev nghttp2-static zlib-static linux-headers \
                                   ca-certificates pkgconfig >/dev/null
                rm -rf build-release-static
                cmake -S . -B build-release-static -GNinja \
                      -DCMAKE_BUILD_TYPE=Release \
                      -DAGENTTY_STANDALONE=ON \
                      -DAGENTTY_FULLY_STATIC=ON >/dev/null
                cmake --build build-release-static -j"$(nproc)"
                strip build-release-static/agentty
            ' > "$dist/.docker-$arch.log" 2>&1 || {
                warn "static build failed for $arch — see $dist/.docker-$arch.log"
                continue
            }
        cp "$root/build-release-static/agentty" "$dist/agentty-linux-$arch"
        chmod +x "$dist/agentty-linux-$arch"
        ok "agentty-linux-$arch ($(du -h "$dist/agentty-linux-$arch" | awk '{print $1}'))"
    done
else
    warn "docker not available — copying existing release binaries from repo root"
    for a in x86_64 aarch64; do
        if [ -x "$root/agentty-linux-$a" ]; then
            cp "$root/agentty-linux-$a" "$dist/"
            ok "agentty-linux-$a (copied from repo root)"
        else
            warn "no agentty-linux-$a anywhere — install docker or pre-build"
        fi
    done
fi

# Windows .exe — we don't cross-build it here; reuse the existing release asset
# if present in the repo root.
if [ -x "$root/agentty-windows-x86_64.exe" ] || [ -f "$root/agentty-windows-x86_64.exe" ]; then
    cp "$root/agentty-windows-x86_64.exe" "$dist/"
    ok "agentty-windows-x86_64.exe (copied)"
elif command -v gh >/dev/null 2>&1; then
    # Pull from previous release so scoop manifest can keep working.
    info "fetching agentty-windows-x86_64.exe from latest release"
    if gh release download --repo 1ay1/agentty --pattern 'agentty-windows-x86_64.exe' \
                          --dir "$dist" --clobber 2>/dev/null; then
        ok "agentty-windows-x86_64.exe (from previous release)"
    else
        skip "no windows binary available"
    fi
fi

# ---- 2. source tarball -------------------------------------------------------
hr "2/6  source tarball"
tarball="agentty-$VERSION.tar.gz"
git -C "$root" archive --format=tar.gz --prefix="agentty-$VERSION/" \
    -o "$dist/$tarball" HEAD
ok "$tarball ($(du -h "$dist/$tarball" | awk '{print $1}'))"

# ---- 3. .deb -----------------------------------------------------------------
hr "3/6  .deb packages"
for pair in 'x86_64 amd64' 'aarch64 arm64'; do
    # shellcheck disable=SC2086
    set -- $pair
    arch=$1; deb_arch=$2
    bin="$dist/agentty-linux-$arch"
    if [ ! -x "$bin" ]; then skip ".deb $deb_arch (no binary)"; continue; fi
    sh "$root/packaging/deb/build.sh" "$VERSION" "$deb_arch" "$bin" "$dist"
done

# ---- 4. .rpm -----------------------------------------------------------------
hr "4/6  .rpm packages"
if command -v rpmbuild >/dev/null 2>&1; then
    for arch in x86_64 aarch64; do
        bin="$dist/agentty-linux-$arch"
        if [ ! -x "$bin" ]; then skip ".rpm $arch (no binary)"; continue; fi
        sh "$root/packaging/rpm/build.sh" "$VERSION" "$arch" "$bin" "$dist"
    done
else
    skip ".rpm (install rpm-tools / rpm package)"
fi

# ---- 5. arch .pkg.tar.zst ----------------------------------------------------
hr "5/6  arch .pkg.tar.zst"
if command -v makepkg >/dev/null 2>&1; then
    # Build natively for whatever arch this host is. Cross-arch makepkg needs
    # a chroot which is out of scope here — but the AUR PKGBUILD itself
    # supports both x86_64 and aarch64 via its source_<arch> arrays.
    host_arch=$(uname -m)
    case "$host_arch" in x86_64|aarch64) ;; *) skip "arch pkg (host=$host_arch)"; host_arch="" ;; esac
    if [ -n "$host_arch" ] && [ -x "$dist/agentty-linux-$host_arch" ]; then
        work=$(mktemp -d)
        # Use a local PKGBUILD that points at the binary in dist/ via file://
        sed -e "s/^pkgver=.*/pkgver=$VERSION/" \
            -e "s|source_${host_arch}=.*|source_${host_arch}=(\"agentty-${VERSION}-${host_arch}::file://$dist/agentty-linux-${host_arch}\")|" \
            -e "s/sha256sums_${host_arch}=.*/sha256sums_${host_arch}=('SKIP')/" \
            "$root/packaging/arch/PKGBUILD" > "$work/PKGBUILD"
        ( cd "$work" && PKGDEST="$dist" makepkg -f --skipinteg --noconfirm \
            2>"$dist/.makepkg.log" >/dev/null ) && \
                ok "arch pkg $(ls "$dist"/*.pkg.tar.zst 2>/dev/null | tail -1 | xargs -I{} basename {})" || \
                warn "makepkg failed — see $dist/.makepkg.log"
        rm -rf "$work"
    else
        skip "arch pkg (binary missing for $host_arch)"
    fi
else
    skip "arch pkg (makepkg not installed)"
fi

# ---- 6. checksums + downstream-package manifests -----------------------------
hr "6/6  checksums + packaging manifests"
( cd "$dist" && sha256sum \
    agentty-linux-x86_64 \
    agentty-linux-aarch64 \
    agentty-windows-x86_64.exe \
    agentty-*.tar.gz \
    agentty_*_amd64.deb \
    agentty_*_arm64.deb \
    agentty-*-1.x86_64.rpm \
    agentty-*-1.aarch64.rpm \
    agentty-*-x86_64.pkg.tar.zst \
    agentty-*-aarch64.pkg.tar.zst \
    2>/dev/null | sort -k2 > SHA256SUMS )
ok "SHA256SUMS"

sum_for() { awk -v f="$1" '$2 == f { print $1 }' "$dist/SHA256SUMS"; }

linux_x64=$(sum_for "agentty-linux-x86_64")
linux_arm=$(sum_for "agentty-linux-aarch64")
win_x64=$(sum_for "agentty-windows-x86_64.exe")
src_tar=$(sum_for "agentty-$VERSION.tar.gz")

# homebrew
if [ -n "$linux_x64" ] && [ -n "$linux_arm" ] && [ -n "$src_tar" ]; then
    sed -e "s/^  version \".*\"/  version \"$VERSION\"/" \
        -e "s|@LINUX_X86_64_SHA256@|$linux_x64|g" \
        -e "s|@LINUX_AARCH64_SHA256@|$linux_arm|g" \
        -e "s|@SRC_TARBALL_SHA256@|$src_tar|g" \
        "$root/packaging/homebrew/agentty.rb" > "$pkgdir/agentty.rb"
    ok "packaging/agentty.rb (homebrew)"
fi

# scoop
if [ -n "$win_x64" ]; then
    sed -e "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" \
        -e "s|/download/v[0-9.]*/|/download/v$VERSION/|g" \
        -e "s|@WINDOWS_X86_64_SHA256@|$win_x64|g" \
        "$root/packaging/scoop/agentty.json" > "$pkgdir/agentty.json"
    ok "packaging/agentty.json (scoop)"
fi

# PKGBUILD (AUR-publishable, with real sha256s pinned)
if [ -n "$linux_x64" ] && [ -n "$linux_arm" ]; then
    sed -e "s/^pkgver=.*/pkgver=$VERSION/" \
        -e "s/sha256sums_x86_64=.*/sha256sums_x86_64=('$linux_x64')/" \
        -e "s/sha256sums_aarch64=.*/sha256sums_aarch64=('$linux_arm')/" \
        "$root/packaging/arch/PKGBUILD" > "$pkgdir/PKGBUILD"
    ok "packaging/PKGBUILD (AUR)"
fi

# ---- summary -----------------------------------------------------------------
hr "summary"
ls -lh "$dist" | grep -v '^total' | grep -v '^d' | awk '{print "    " $9 "  " $5}' | sort

# ---- upload ------------------------------------------------------------------
if [ "$UPLOAD" -eq 1 ]; then
    hr "uploading $TAG via gh"
    command -v gh >/dev/null || { echo "gh not installed"; exit 1; }
    if gh release view "$TAG" --repo 1ay1/agentty >/dev/null 2>&1; then
        info "release $TAG exists — uploading assets with --clobber"
        gh release upload "$TAG" --repo 1ay1/agentty --clobber \
            "$dist"/agentty-* "$dist"/SHA256SUMS
    else
        info "creating release $TAG"
        gh release create "$TAG" --repo 1ay1/agentty \
            --title "agentty $TAG" \
            --notes "See [CHANGELOG.md](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md)." \
            "$dist"/agentty-* "$dist"/SHA256SUMS
    fi
    ok "uploaded"
fi

hr "done"
