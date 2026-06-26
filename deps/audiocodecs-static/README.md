# Vendored audio codecs (Ogg Vorbis + Opus) — static, App Store-safe

`audiocodecs.xcframework` is a **statically-linked** build of the open-source
Xiph decoders the Workspace **Music** applet uses to play formats AVFoundation
can't decode on its own:

- **libogg** 1.3.5 — Ogg container
- **libvorbis** 1.3.7 — Ogg Vorbis (`vorbisfile`)
- **libopus** 1.5.2 + **libopusfile** 0.12 — Opus / Ogg Opus

Everything else (MP3, AAC/M4A, ALAC, WAV, AIFF, CAF, FLAC) is decoded by the
system `AVAudioFile`, so this framework exists **only** for `.ogg`/`.opus`.

## Why static / why this exists

Same reasoning as `deps/liblzma-static`: these are **public** open-source
libraries, but they are not part of iOS. Building them ourselves and linking
**statically** means the `ov_*` / `op_*` symbols are defined in our own binary —
no dependency on any private system library, and nothing for App Store binary
validation to flag. No Apple SPI is used anywhere.

## How the app consumes it

`app/AudioPCMDecoder_Vorbis.m` and `app/AudioPCMDecoder_Opus.m` guard their real
implementation behind `#if __has_include(<vorbis/vorbisfile.h>)` /
`<opusfile.h>`. So:

- **Before** this xcframework is built/linked, the app still compiles and plays
  every native format; `.ogg`/`.opus` simply report "support not built" and are
  hidden from the file picker.
- **After** it's linked, the Ogg/Opus backends light up automatically and the
  picker starts advertising those extensions.

## Build / rebuild

One-time step on a Mac with Xcode (downloads the Xiph release tarballs into
`src/` on first run):

```sh
cd deps/audiocodecs-static
./build-audiocodecs-apple.sh
```

This produces `audiocodecs.xcframework` with `ios-arm64` (device) and
`ios-arm64_x86_64-simulator` slices, plus a flat `Headers/` (`ogg/`, `vorbis/`,
`opus/`). Commit the resulting `audiocodecs.xcframework` (like `liblzma.xcframework`).

## Xcode wiring (already in the project file)

- `audiocodecs.xcframework` is added under **Link Binary With Libraries** for the
  app (and FileProvider, matching liblzma).
- The framework's `Headers` dir is on the header search path via the xcframework,
  so `<vorbis/vorbisfile.h>` and `<opus/opusfile.h>` resolve.

## Self-contained check (no leaks to system libs)

```sh
A=audiocodecs.xcframework/ios-arm64/audiocodecs-ios-arm64.a
comm -23 \
  <(nm "$A" | awk 'NF==2 && $1=="U" && $2 ~ /^_(ov_|op_|opus_|ogg_|vorbis_)/{print $2}' | sort -u) \
  <(nm "$A" | awk 'NF==3 && $3 ~ /^_(ov_|op_|opus_|ogg_|vorbis_)/{print $3}' | sort -u)
# empty == self-contained
```
