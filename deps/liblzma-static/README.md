# Vendored liblzma (xz) — static, App Store-safe

`liblzma.xcframework` here is a **statically-linked** build of **liblzma 5.8.3**
(the xz library), compiled from the source vendored in `src/`. The app and the
FileProvider extension link it instead of the iOS **system** `/usr/lib/liblzma.tbd`.

## Why this exists

iOS ships `liblzma.dylib`, but it is a **private** library. Linking the system
`liblzma.tbd` made the app *import* the `lzma_*` symbols from that private dylib,
which builds and runs in the simulator but is **rejected by App Store binary
validation**:

> The app references non-public symbols … _lzma_code, _lzma_crc32, _lzma_end,
> _lzma_alone_decoder, _lzma_raw_decoder, _lzma_properties_decode,
> _lzma_stream_decoder

By compiling our own liblzma and linking it statically, those symbols become
**defined in our binary** instead of undefined references to a private system
library, so validation passes. libarchive's xz reader (used to import `.tar.xz`
roots) keeps working unchanged.

## What's here

- `src/` — faithful subset of the xz 5.8.3 `src/liblzma` tree (git tag `v5.8.3`,
  commit `4b73f2ec19a99ef465282fbce633e8deb33691b3`) plus the `tuklib_*` files
  from `src/common`. Source is taken from the **git tag**, not a release
  tarball, to avoid the CVE-2024-3094 tarball-only build artifacts (that
  backdoor was never in the git source, and never in 5.8.x regardless).
- `config.h` — hand-written replacement for the autotools-generated config:
  Apple platforms, **decoders + encoders**, **single-threaded**, size-optimized
  (runtime CRC/probability tables, no generated table headers).
- `build-liblzma-apple.sh` — cross-compiles a slice and link-tests it.
- `liblzma.xcframework` — the built product: `ios-arm64` (device) and
  `ios-arm64_x86_64-simulator` slices.

## Rebuild

```sh
cd deps/liblzma-static
./build-liblzma-apple.sh iphoneos        arm64  -mios-version-min=12.0           /tmp/liblzma-ios-arm64.a
./build-liblzma-apple.sh iphonesimulator arm64  -mios-simulator-version-min=12.0 /tmp/liblzma-sim-arm64.a
./build-liblzma-apple.sh iphonesimulator x86_64 -mios-simulator-version-min=12.0 /tmp/liblzma-sim-x86_64.a
lipo -create /tmp/liblzma-sim-arm64.a /tmp/liblzma-sim-x86_64.a -output /tmp/liblzma-sim.a
rm -rf liblzma.xcframework
xcodebuild -create-xcframework \
  -library /tmp/liblzma-ios-arm64.a -headers src/api \
  -library /tmp/liblzma-sim.a       -headers src/api \
  -output liblzma.xcframework
```

The build script's link-test fails the build if any symbol libarchive needs is
missing. To confirm the framework leaks nothing to the system library:

```sh
A=liblzma.xcframework/ios-arm64/liblzma-ios-arm64.a   # (the embedded archive)
comm -23 \
  <(nm "$A" | awk 'NF==2 && $1=="U" && $2 ~ /^_lzma_/{print $2}' | sort -u) \
  <(nm "$A" | awk 'NF==3 && $3 ~ /^_lzma_/{print $3}' | sort -u)
# empty output == self-contained
```

## Notes

- Decoders + encoders: the app never *encodes* xz at runtime, but libarchive's
  xz/7zip/xar **writer** object files reference the `lzma_*_encoder` entry points
  at link time (they compile under `HAVE_LZMA_H`, which the xz reader requires),
  so the encoders must be built — otherwise the app and the FileProvider
  extension fail to link with `Undefined symbol: _lzma_stream_encoder` (and
  `_lzma_lzma_preset`, `_lzma_raw_encoder`, `_lzma_alone_encoder`,
  `_lzma_properties_encode`, `_lzma_properties_size`). The **threaded** encoder
  (`stream_encoder_mt.c`) is still excluded: libarchive only calls it under
  `HAVE_LZMA_STREAM_ENCODER_MT`, which `deps/config.h` leaves undefined.
- The public API headers libarchive compiles against still live in
  `deps/liblzma/` (unchanged); the copy under `src/api/` here is for rebuilding.
- `HAVE_LZMA_H` / `HAVE_LIBLZMA` in `deps/config.h` stay defined — libarchive's
  xz reader now resolves against this static lib rather than the system dylib.
