#!/bin/bash
# Cross-compile static libogg / libvorbis / libopus / libopusfile for Apple and
# assemble a single App-Store-safe audiocodecs.xcframework (ios-arm64 device +
# ios-arm64_x86_64 simulator). These are the open-source Xiph decoders — public
# code only, no Apple private interfaces — used by the Workspace audio player to
# play Ogg Vorbis and Opus (formats AVFoundation can't decode itself).
#
# One-time vendoring step, exactly like deps/liblzma-static/build-liblzma-apple.sh.
# Run once on a Mac with Xcode; commit the resulting audiocodecs.xcframework.
#
#   cd deps/audiocodecs-static && ./build-audiocodecs-apple.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
OUT="$ROOT/lib"
HEADERS="$ROOT/Headers"
MIN=12.0

OGG_VER=1.3.5
VORBIS_VER=1.3.7
OPUS_VER=1.5.2
OPUSFILE_VER=0.12

mkdir -p "$SRC" "$OUT"

fetch() { # name url
  local tarball="$SRC/$(basename "$2")"
  local dir="$SRC/$1"
  if [ ! -d "$dir" ]; then
    [ -f "$tarball" ] || { echo "  downloading $1 ..."; curl -fL "$2" -o "$tarball"; }
    tar -xzf "$tarball" -C "$SRC"
  fi
}

echo "== fetching sources =="
fetch "libogg-$OGG_VER"       "https://downloads.xiph.org/releases/ogg/libogg-$OGG_VER.tar.gz"
fetch "libvorbis-$VORBIS_VER" "https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS_VER.tar.gz"
fetch "opus-$OPUS_VER"        "https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
fetch "opusfile-$OPUSFILE_VER" "https://downloads.xiph.org/releases/opus/opusfile-$OPUSFILE_VER.tar.gz"

# Build every Xiph lib for one slice into a private staging prefix, then merge
# the static archives into one $OUT/audiocodecs-<tag>.a.
build_slice() { # sdk arch host tag minflag
  local sdk="$1" arch="$2" host="$3" tag="$4" minflag="$5"
  local sysroot; sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  local stage="$ROOT/stage-$tag"
  rm -rf "$stage"; mkdir -p "$stage"

  export CC; CC="$(xcrun -f clang)"
  export CFLAGS="-arch $arch -isysroot $sysroot $minflag -O2 -fPIC -fvisibility=hidden"
  export LDFLAGS="-arch $arch -isysroot $sysroot $minflag"
  export PKG_CONFIG_PATH="$stage/lib/pkgconfig"
  local common=(--host="$host" --prefix="$stage" --enable-static --disable-shared --disable-dependency-tracking)

  echo "== $tag: libogg =="
  ( cd "$SRC/libogg-$OGG_VER" && ./configure "${common[@]}" >/dev/null && make -j >/dev/null && make install >/dev/null )

  echo "== $tag: libvorbis =="
  ( cd "$SRC/libvorbis-$VORBIS_VER" && ./configure "${common[@]}" --with-ogg="$stage" >/dev/null && make -j >/dev/null && make install >/dev/null )

  echo "== $tag: opus =="
  ( cd "$SRC/opus-$OPUS_VER" && ./configure "${common[@]}" --disable-extra-programs --disable-doc >/dev/null && make -j >/dev/null && make install >/dev/null )

  echo "== $tag: opusfile =="
  # --disable-http drops the (TLS/openssl) URL-streaming feature we don't use, so
  # there is no openssl dependency to vendor.
  ( cd "$SRC/opusfile-$OPUSFILE_VER" && ./configure "${common[@]}" --disable-http --disable-examples --disable-doc \
        --with-ogg="$stage" >/dev/null && make -j >/dev/null && make install >/dev/null )

  echo "== $tag: merging static archives =="
  libtool -static -o "$OUT/audiocodecs-$tag.a" \
    "$stage/lib/libogg.a" "$stage/lib/libvorbis.a" "$stage/lib/libvorbisfile.a" \
    "$stage/lib/libopus.a" "$stage/lib/libopusfile.a"

  # Stage headers once (identical across slices): ogg/, vorbis/, opus/.
  if [ ! -d "$HEADERS" ]; then
    mkdir -p "$HEADERS"
    cp -R "$stage/include/." "$HEADERS/"
  fi
}

rm -rf "$HEADERS"
build_slice iphoneos        arm64  aarch64-apple-darwin ios-arm64     "-mios-version-min=$MIN"
build_slice iphonesimulator arm64  aarch64-apple-darwin sim-arm64     "-mios-simulator-version-min=$MIN"
build_slice iphonesimulator x86_64 x86_64-apple-darwin  sim-x86_64    "-mios-simulator-version-min=$MIN"

echo "== lipo simulator slices =="
lipo -create "$OUT/audiocodecs-sim-arm64.a" "$OUT/audiocodecs-sim-x86_64.a" -output "$OUT/audiocodecs-sim.a"

echo "== create xcframework =="
rm -rf "$ROOT/audiocodecs.xcframework"
xcodebuild -create-xcframework \
  -library "$OUT/audiocodecs-ios-arm64.a" -headers "$HEADERS" \
  -library "$OUT/audiocodecs-sim.a"        -headers "$HEADERS" \
  -output "$ROOT/audiocodecs.xcframework"

echo "done -> $ROOT/audiocodecs.xcframework"
