# Release Notes Since `builds/iSH-AOK_533`

These notes summarize changes from `builds/iSH-AOK_533` intended for `builds/iSH-AOK_534`.

> Work in progress. Other post-533 commits already on `working` — vendored static
> liblzma (encoders added), the exec-loader EOF-straddle `SIGBUS` fix, the
> `/proc/meminfo` `host_statistics64` crash fix, and the `file_lock_from_flock`
> NULL guard — will be folded in at tag time. This file currently covers the new
> Workspace Music player.

## Highlights

- **New Workspace "Music" player applet.** A native audio/music player in the Workspace — playlists, a now-playing scrubber, shuffle/repeat, and background playback with lock-screen / Control Center controls. Plays the system-native formats (MP3, AAC/M4A, ALAC, WAV, AIFF, CAF, FLAC) out of the box, with **Ogg Vorbis and Opus** added via vendored open-source decoders (no private Apple APIs).

## User-Facing Changes

### Workspace — Music Player

A new **Music** applet, opened from the Workspace root/Utilities menu (Workspace group), plays audio from the guest filesystem:

- **Library.** Defaults to `/AOK/persist/music` (a real-fs mount, so it survives root switches), with **Add from Path…** to queue a single track or a whole folder from anywhere in the guest. Files inside a root's fakefs are read through the emulated VFS, so they play even though they aren't host files.
- **Playback.** Now-playing title/artist, a draggable scrubber, play/pause, previous/next, shuffle, and repeat (off / all / one); volume persists. Playback is independent of the window — closing the Music window does not stop the music.
- **Background + lock screen.** Audio keeps playing when iSH-AOK is backgrounded or the device is locked, with play/pause/skip and scrubbing from the lock screen and Control Center (new `audio` background mode).
- **Playlists.** Save the current queue as a named playlist and reload it later (stored as JSON under `/AOK/persist/playlists`).
- **Formats.** MP3, AAC/M4A, ALAC, WAV, AIFF, CAF, and FLAC decode natively via `AVAudioFile`; `.ogg` (Vorbis) and `.opus` decode via the vendored libvorbis/libopus.

## Maintainer Notes

- **Vendored audio codecs (App Store-safe), mirroring liblzma.** Ogg/Opus use the open-source Xiph decoders (libogg/libvorbis/libopus/opusfile), statically linked via `deps/audiocodecs-static/audiocodecs.xcframework` — **no Apple private interfaces** (the same reasoning as the vendored liblzma). One-time build: `deps/audiocodecs-static/build-audiocodecs-apple.sh` downloads the Xiph release tarballs and cross-compiles `ios-arm64` + `ios-arm64_x86_64-simulator`; commit the resulting `audiocodecs.xcframework` + `Headers/` like liblzma.
- **Graceful degradation.** The Ogg/Opus decoder backends are gated by `__has_include`, so the app compiles and plays every native format **before** the codec xcframework exists; `.ogg`/`.opus` are simply hidden from the picker until it's linked. After building the xcframework, re-run `ruby tools/wire_audio_player.rb` to link it.
- **Layout.** New `app/` files (`AudioPlayerEngine`, `AudioPCMDecoder{,_AVF,_Vorbis,_Opus}`, `AudioLibrary`) compile in `libiSH-AOKApp`; the applet view controller lives in `WorkspaceViewController.m`. AVFoundation + MediaPlayer link in `iSH-AOK` / `iSH-AOK+Linux`. The pbxproj wiring (sources, frameworks, codec header search path) is done idempotently by `tools/wire_audio_player.rb` via the `xcodeproj` gem.
- All new Objective-C compiles clean (`clang -fsyntax-only` against the iOS SDK); needs an app rebuild to reach the device and has not yet had an on-device runtime pass (the streaming buffer pipeline, scrubbing, and lock-screen controls).
