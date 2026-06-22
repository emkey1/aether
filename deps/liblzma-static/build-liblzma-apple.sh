#!/bin/bash
# Cross-compile a single-threaded liblzma 5.8.3 static slice (decoders + encoders)
# for an Apple SDK/arch, then prove completeness by link-testing the symbols
# libarchive needs. Encoders are built because libarchive's xz/7zip/xar writer
# object files reference the lzma_*_encoder entry points at link time (they
# compile under HAVE_LZMA_H, required by the xz reader) — see README.md.
# Usage: build.sh <sdk> <arch> <min-version-flag> <out.a>
set -euo pipefail
SDK="$1"; ARCH="$2"; MINFLAG="$3"; OUT="$4"
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
SYSROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
CC="$(xcrun -f clang)"
WORK="$ROOT/obj-$SDK-$ARCH"
rm -rf "$WORK"; mkdir -p "$WORK"

INCS=(-I"$ROOT" -I"$SRC/api" -I"$SRC/common" -I"$SRC/check" -I"$SRC/lz" \
      -I"$SRC/lzma" -I"$SRC/rangecoder" -I"$SRC/delta" -I"$SRC/simple" -I"$SRC/tuklib")
CFLAGS=(-arch "$ARCH" -isysroot "$SYSROOT" "$MINFLAG" -O2 -fPIC -fvisibility=hidden \
        -DHAVE_CONFIG_H -DTUKLIB_SYMBOL_PREFIX=lzma_ -Wno-implicit-function-declaration "${INCS[@]}")

# Source allowlist (single-threaded; mirrors what xz's --disable-threads builds).
# Decoder set (used at runtime to import .tar.xz roots).
FILES=(
  common/common.c common/alone_decoder.c common/auto_decoder.c
  common/block_buffer_decoder.c common/block_decoder.c common/block_header_decoder.c
  common/block_util.c common/filter_buffer_decoder.c
  common/filter_common.c common/filter_decoder.c common/filter_flags_decoder.c
  common/index.c common/index_decoder.c common/index_hash.c
  common/stream_buffer_decoder.c common/stream_decoder.c common/stream_flags_common.c
  common/stream_flags_decoder.c common/vli_decoder.c common/vli_size.c
  common/hardware_physmem.c common/lzip_decoder.c common/microlzma_decoder.c
  check/check.c check/crc32_small.c check/crc64_small.c check/sha256.c
  lz/lz_decoder.c
  lzma/lzma_decoder.c lzma/lzma2_decoder.c
  delta/delta_common.c delta/delta_decoder.c
  simple/simple_coder.c simple/simple_decoder.c simple/x86.c simple/arm.c
  simple/armthumb.c simple/arm64.c simple/powerpc.c simple/sparc.c simple/ia64.c simple/riscv.c
  tuklib/tuklib_physmem.c tuklib/tuklib_cpucores.c
)
# Encoder set (not used at runtime, but libarchive's xz/7zip/xar writers reference
# the lzma_*_encoder entry points at link time — see README.md). Excludes the
# threaded encoder (stream_encoder_mt.c) and the !HAVE_SMALL table generators
# (fastpos_table.c / *_tablegen.c). price_table.c IS required: price.h declares
# lzma_rc_prices as an unconditional `extern const`.
FILES+=(
  common/alone_encoder.c common/block_buffer_encoder.c common/block_encoder.c
  common/block_header_encoder.c common/easy_buffer_encoder.c common/easy_encoder.c
  common/easy_encoder_memusage.c common/easy_preset.c common/filter_buffer_encoder.c
  common/filter_encoder.c common/filter_flags_encoder.c common/index_encoder.c
  common/microlzma_encoder.c common/stream_buffer_encoder.c common/stream_encoder.c
  common/stream_flags_encoder.c common/vli_encoder.c
  lz/lz_encoder.c lz/lz_encoder_mf.c
  lzma/lzma_encoder.c lzma/lzma_encoder_optimum_fast.c lzma/lzma_encoder_optimum_normal.c
  lzma/lzma_encoder_presets.c lzma/lzma2_encoder.c
  rangecoder/price_table.c
  delta/delta_encoder.c
  simple/simple_encoder.c
)

echo "  compiling ${#FILES[@]} files for $SDK/$ARCH ..."
OBJS=()
for f in "${FILES[@]}"; do
  o="$WORK/$(echo "$f" | tr / _).o"
  "$CC" "${CFLAGS[@]}" -c "$SRC/$f" -o "$o"
  OBJS+=("$o")
done
xcrun -f ar >/dev/null
rm -f "$OUT"   # ar 'r' replaces but never removes stale members — rebuild fresh
xcrun --sdk "$SDK" ar rcs "$OUT" "${OBJS[@]}"
echo "  -> $OUT ($(du -h "$OUT" | cut -f1))"

# Completeness check: link-test referencing the exact symbols App Store flagged + the
# libarchive xz-filter entry points. Link only (no run); undefined refs => failure.
cat > "$WORK/linktest.c" <<'EOF'
#include <lzma.h>
extern void *use(void*);
int probe(void) {
    lzma_stream s = LZMA_STREAM_INIT;
    /* decoder entry points (xz reader) */
    use((void*)&lzma_stream_decoder); use((void*)&lzma_alone_decoder);
    use((void*)&lzma_raw_decoder);    use((void*)&lzma_properties_decode);
    use((void*)&lzma_code);           use((void*)&lzma_end);
    use((void*)&lzma_crc32);          use((void*)&lzma_auto_decoder);
    /* encoder entry points referenced by libarchive's xz/7zip/xar writers */
    use((void*)&lzma_stream_encoder); use((void*)&lzma_alone_encoder);
    use((void*)&lzma_raw_encoder);    use((void*)&lzma_lzma_preset);
    use((void*)&lzma_properties_encode); use((void*)&lzma_properties_size);
    use((void*)&s);
    return 0;
}
int main(void){return probe();}
EOF
echo 'void *use(void*p){return p;}' > "$WORK/use.c"
"$CC" -arch "$ARCH" -isysroot "$SYSROOT" "$MINFLAG" -I"$SRC/api" \
    "$WORK/linktest.c" "$WORK/use.c" "$OUT" -o "$WORK/linktest" \
    -Wl,-undefined,error
echo "  link-test PASSED (all required lzma_* symbols resolved from $OUT)"
