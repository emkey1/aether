#!/bin/sh
# build.sh -- build (and optionally install) ktop from the read-only source
# shipped at /AOK/tools/ktop.
#
# /AOK is a read-only mount, so this copies the source to a writable work
# directory first (default /tmp/ktop-build), builds it there with make, and
# leaves the resulting binary at $WORK_DIR/ktop. Pass "install" to also copy
# it to $PREFIX/bin/ktop (default /usr/local/bin) so it's on PATH.
#
# Usage:
#   sh /AOK/tools/ktop/build.sh              # build only
#   sh /AOK/tools/ktop/build.sh install       # build + install
#
# Environment:
#   WORK_DIR   Work directory. Default: /tmp/ktop-build
#   PREFIX     Install prefix (used with "install"). Default: /usr/local
#   CC         Compiler passed to make. Default: cc
set -eu

SRC_DIR=$(dirname "$0")
WORK_DIR="${WORK_DIR:-/tmp/ktop-build}"
PREFIX="${PREFIX:-/usr/local}"
CC="${CC:-cc}"

mkdir -p "$WORK_DIR"
cp "$SRC_DIR/ktop.c" "$SRC_DIR/Makefile" "$WORK_DIR/"

echo "Building ktop in $WORK_DIR ..."
make -C "$WORK_DIR" CC="$CC"

echo "Built: $WORK_DIR/ktop"

if [ "${1:-}" = install ]; then
    echo "Installing to $PREFIX/bin/ktop ..."
    make -C "$WORK_DIR" install PREFIX="$PREFIX"
    echo "Installed. Run: ktop"
else
    echo "Run it directly: $WORK_DIR/ktop"
    echo "Or install it to PATH: sh $0 install"
fi
