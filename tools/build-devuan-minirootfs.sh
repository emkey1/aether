#!/bin/sh
# build-devuan-minirootfs.sh
# ---------------------------------------------------------------------------
# Build *minimal* Devuan 6 "Excalibur" rootfs tarballs for iSH-AOK, the Devuan
# analog of the bundled `alpine-minirootfs-3.23.3-x86{,_64}.tar.gz` images.
#
# Produces (in OUT_DIR, default = repo root):
#   devuan-minirootfs-6.0-x86.tar.gz      i386  (i686)
#   devuan-minirootfs-6.0-x86_64.tar.gz   amd64 (x86_64)
#
# Strategy
# --------
# We bootstrap straight from the Devuan "merged" archive with `mmdebstrap
# --variant=minbase`, which yields a bare apt/dpkg/glibc/coreutils/bash system
# -- exactly like the Alpine minirootfs -- so that provision-ultimate-devuan.sh
# can do all the "ultimate terminal" heavy lifting on top.
#
# mmdebstrap runs inside a *matching-architecture* Debian container: on this
# arm64 host Docker emulates linux/386 and linux/amd64 via binfmt/qemu, and by
# matching the container arch to the target rootfs arch the package maintainer
# scripts run "native" to the (already emulated) container -- no fragile nested
# qemu-user binfmt setup. The Devuan archive keyring is lifted out of the
# official `dyne/devuan:excalibur` image so the Release signature verifies.
#
# This keeps the same provenance philosophy as the existing amd64 docker-export
# images (Linux-native uid/gid/perms in the tar) while being (a) genuinely
# minimal and (b) buildable for i386, which the amd64-only dyne image is not.
#
# Usage:
#   tools/build-devuan-minirootfs.sh                # both arches
#   ARCHES=i386 tools/build-devuan-minirootfs.sh    # just one
#   OUT_DIR=/tmp tools/build-devuan-minirootfs.sh   # elsewhere
#
# Tunables (env):
#   ARCHES         space list of deb arches to build   (default "i386 amd64")
#   SUITE          Devuan suite                         (default excalibur)
#   VERSION        version string used in filenames     (default 6.0)
#   DEVUAN_MIRROR  archive mirror                        (default deb.devuan.org)
#   BUILD_IMAGE    Debian image that hosts mmdebstrap   (default debian:trixie)
#   OUT_DIR        where the .tar.gz land               (default repo root)
#   KEEP_APT_LISTS 1 to keep /var/lib/apt/lists (bigger) (default unset = strip)
# ---------------------------------------------------------------------------
set -eu

SUITE="${SUITE:-excalibur}"
VERSION="${VERSION:-6.0}"
DEVUAN_MIRROR="${DEVUAN_MIRROR:-http://deb.devuan.org/merged}"
BUILD_IMAGE="${BUILD_IMAGE:-debian:trixie}"
ARCHES="${ARCHES:-i386 amd64}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${OUT_DIR:-$REPO_ROOT}"
KEYRING_IMAGE="${KEYRING_IMAGE:-dyne/devuan:excalibur}"

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found in PATH"
docker info >/dev/null 2>&1 || die "docker daemon is not running"

# --- output compression -----------------------------------------------------
# iSH-AOK imports via libarchive (archive_read_support_filter_all in
# tools/fakefs.c), which reads gzip/xz/zstd/bzip2 transparently, and the import
# decompresses *natively* and only once -- so the smallest file wins. xz -9e is
# ~39% smaller than gzip on this rootfs (30 MB vs 50 MB). Override with
# COMPRESS=zst (faster, ~32 MB) or COMPRESS=gz.
COMPRESS="${COMPRESS:-xz}"
case "$COMPRESS" in
    xz)  COMP_EXT="tar.xz";  COMP_TOOL="xz";   COMP_CMD="xz -9e -c" ;;
    zst) COMP_EXT="tar.zst"; COMP_TOOL="zstd"; COMP_CMD="zstd -19 -c" ;;
    gz)  COMP_EXT="tar.gz";  COMP_TOOL="gzip"; COMP_CMD="gzip -9 -c" ;;
    *)   die "unknown COMPRESS='$COMPRESS' (use one of: xz zst gz)" ;;
esac
command -v "$COMP_TOOL" >/dev/null 2>&1 || die "compressor '$COMP_TOOL' not found in PATH"

# i386 -> x86, amd64 -> x86_64 (match the Alpine minirootfs filename convention).
arch_suffix() {
    case "$1" in
        i386)  echo x86 ;;
        amd64) echo x86_64 ;;
        *)     echo "$1" ;;
    esac
}
# deb arch -> docker --platform value
arch_platform() {
    case "$1" in
        i386)  echo linux/386 ;;
        amd64) echo linux/amd64 ;;
        *)     die "unknown arch '$1'" ;;
    esac
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/devuan-minirootfs.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT_DIR"

# --- Devuan archive keyring (extracted from the official Devuan image) --------
log "Fetching the Devuan archive keyring from $KEYRING_IMAGE"
docker pull --platform=linux/amd64 "$KEYRING_IMAGE" >/dev/null
docker run --rm --platform=linux/amd64 "$KEYRING_IMAGE" \
    cat /usr/share/keyrings/devuan-archive-keyring.gpg > "$WORK/devuan-archive-keyring.gpg"
[ -s "$WORK/devuan-archive-keyring.gpg" ] || die "could not extract the Devuan keyring"
note "keyring: $(wc -c < "$WORK/devuan-archive-keyring.gpg") bytes"

# Optionally drop the apt package lists to shrink the image (the guest will run
# `apt-get update` anyway, exactly like Alpine needs `apk update`).
CLEAN_HOOK='rm -rf "$1"/var/lib/apt/lists/* "$1"/var/cache/apt/archives/*.deb "$1"/var/cache/apt/*.bin 2>/dev/null || true'
[ "${KEEP_APT_LISTS:-}" = 1 ] && CLEAN_HOOK='true'

build_one() {  # <deb-arch>
    arch="$1"
    suffix="$(arch_suffix "$arch")"
    platform="$(arch_platform "$arch")"
    out="$OUT_DIR/devuan-minirootfs-$VERSION-$suffix.$COMP_EXT"

    log "Building $arch ($platform) -> ${out##*/}"
    # mmdebstrap writes an uncompressed tar inside the container; we compress it
    # ($COMPRESS) on the host for the smallest possible bundle.
    docker run --rm --privileged --platform="$platform" \
        -e SUITE="$SUITE" -e ARCH="$arch" -e MIRROR="$DEVUAN_MIRROR" \
        -e CLEAN_HOOK="$CLEAN_HOOK" \
        -v "$WORK:/work" \
        "$BUILD_IMAGE" bash -euc '
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends mmdebstrap >/dev/null
            # mmdebstrap drives apt with a Pre-Install-Pkgs hook ("cat >&14")
            # that redirects to a 2-digit fd. The container /bin/sh is dash,
            # which cannot parse ">&14" ("Bad fd number"); bash can. Point
            # /bin/sh at bash so the apt hook works (chroot maintainer scripts
            # run the *target* /bin/sh, so this only affects the outer hook).
            ln -sf /bin/bash /bin/sh
            : "${ARCH:?}" "${SUITE:?}" "${MIRROR:?}"
            echo "    mmdebstrap $(mmdebstrap --version 2>/dev/null | head -1)"
            mmdebstrap \
                --mode=root \
                --variant=minbase \
                --format=tar \
                --components=main \
                --arch="$ARCH" \
                --keyring=/work/devuan-archive-keyring.gpg \
                --include=ca-certificates,devuan-keyring \
                --aptopt="Acquire::Check-Valid-Until \"false\"" \
                --customize-hook="$CLEAN_HOOK" \
                "$SUITE" "/work/rootfs-$ARCH.tar" \
                "deb $MIRROR $SUITE main"
        '

    [ -s "$WORK/rootfs-$arch.tar" ] || die "mmdebstrap produced no tar for $arch"
    $COMP_CMD "$WORK/rootfs-$arch.tar" > "$out"
    rm -f "$WORK/rootfs-$arch.tar"

    # --- sanity: read os-release + arch back out of the finished tarball ------
    # tar -xO without an explicit filter: bsdtar/libarchive auto-detects xz/gz/zst.
    note "wrote $out ($(ls -lh "$out" | awk '{print $5}'))"
    osline="$(tar -xOf "$out" ./usr/lib/os-release 2>/dev/null | grep -E '^PRETTY_NAME=' || true)"
    dpkgarch="$(tar -xOf "$out" ./var/lib/dpkg/arch 2>/dev/null | head -1 || true)"
    note "  ${osline:-<no os-release>}"
    note "  dpkg arch: ${dpkgarch:-<unknown>}"
    [ "$dpkgarch" = "$arch" ] || note "  WARNING: dpkg arch '$dpkgarch' != requested '$arch'"
}

for a in $ARCHES; do
    build_one "$a"
done

log "Done"
note "Images in $OUT_DIR:"
for a in $ARCHES; do
    f="$OUT_DIR/devuan-minirootfs-$VERSION-$(arch_suffix "$a").$COMP_EXT"
    [ -f "$f" ] && note "  $(ls -lh "$f" | awk '{print $5"  "$NF}')"
done
cat <<EOF

    Next:
      * Smoke-test locally:
          (cd build && ./tools/fakefsify $OUT_DIR/devuan-minirootfs-$VERSION-x86.$COMP_EXT devuan-x86)
          ./build/ish -f build/devuan-x86 /bin/sh
      * Provision into a full system from inside the guest:
          sh /opt/AOK/tools/provision-ultimate-devuan.sh
EOF
