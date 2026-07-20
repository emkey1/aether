# Workspace File Manager + Viewer Applets: Design Plan

Goal: a Finder-style file manager applet for Workspace mode, plus three viewer
applets (markdown reader, image viewer, video player), with the existing Music
applet wired in as the handler for audio files. Everything is native UIKit,
following the established applet model (WorkspaceThemedToolViewController
subclasses inside ISHWorkspaceContainedWindowView windows).

Non-goals for v1: guest-process integration (this is a native applet, not a
guest `mc`), ffmpeg/software video decode (AVFoundation formats only), Files.app
drag-out (the FileProvider integration covers external access), tags/labels,
Spotlight-style content search.

---

## Layer 0: Shared guest VFS bridge (`app/GuestFileBridge.{h,m}`)

The single biggest piece of groundwork. Today the "borrow pid 1 as `current`,
call generic_open/readdir through the fs/ layer" pattern is duplicated in
`AudioLibrary.m:167` and `MotePadDocumentStore.m:147`, and a third richer copy
lives in the FileProvider extension. A fourth copy for the file manager would be
a maintenance bug factory. Factor it into one class all applets share.

### API shape

`ISHGuestFileBridge` singleton, all operations async on a private serial
`ioQueue`, completions on main. Never call VFS ops on the main thread (they
block on fakefs SQLite and inodes_lock). Every call fails cleanly with a
distinct error when pid 1 is not yet usable, so applets can render a
"guest not booted" empty state instead of wedging.

Item model `ISHGuestFileItem`: name, full guest path, kind (regular / dir /
symlink / fifo / socket / chardev / blockdev), size, mtime, mode bits,
uid/gid, symlink target, and `isRealfsBacked` + resolved host URL when the
path lives under `/AOK/persist` (AppGroup fast path, same logic as
`hostURLForPersistGuestPath:` in MotePad/AudioLibrary).

Operations (all built on `generic_*` from fs/, wrapped in the borrowed-task
context helper):

- `listDirectory(path)` -> [ISHGuestFileItem], stat'd per entry. Use
  `O_NONBLOCK_` when opening anything of unknown type and classify with stat
  first so FIFOs/devices never block the queue (MotePadDocumentStore.m:200
  already does this correctly; copy that behavior).
- `stat(path)` / `readlink(path)`.
- `readFile(path, maxBytes)` -> NSData. Hard cap parameter is mandatory;
  callers must not slurp unbounded files (jetsam).
- `extractToTemp(path, progress, cancel)` -> host file URL. Streamed
  generic read -> host write in ~1 MiB chunks, progress callback, cancelable
  between chunks. Cache keyed by (guest path, size, mtime) with an LRU byte
  cap (~512 MiB) and cleanup at app launch. This generalizes
  `extractGuestFileToTemp` in AudioLibrary.m:250.
- `writeFile(path, data)` atomic via temp + `generic_renameat` (lift MotePad's
  implementation, MotePadDocumentStore.m:483).
- `mkdir`, `rename/move`, `unlink`, `rmdirRecursive` (depth-first walk with a
  visited-inode guard against symlink games; never follow symlinks while
  deleting), `copy` (read/write loop; realfs->realfs uses NSFileManager),
  `chmod` (for Get Info, phase 4).
- `openFdForRandomAccess(path)` -> opaque handle with `pread(offset, len)` and
  explicit close, still serialized on ioQueue. This is what the video
  resource-loader path (phase 3b) needs.

Errno mapping: promote `NSError+ISHErrno` out of the FileProvider target into
the main app so every completion carries a real NSError with the guest errno.

### Task-context correctness

Keep the exact `withGuestTaskContext:` semantics (take pid 1 ref, verify
mm/mem/files/fs non-NULL under general_lock, set thread-local `current`,
restore, drop ref). One subtlety to preserve: `current` is thread-local, and
the bridge's serial queue guarantees one VFS op at a time, so there is no
cross-thread `current` leakage. If we later want parallel reads (thumbnails),
add a second queue that does its own borrow rather than sharing state.

Migration: AudioLibrary and MotePadDocumentStore adopt the bridge in phase 5.
Not a v1 blocker, but the plan of record so the duplicates die.

### Source layout rule

None of this goes into WorkspaceViewController.m (12.9k lines and growing).
Each new applet gets its own translation unit:

- `app/GuestFileBridge.{h,m}` (kernel headers live here, not in UI files)
- `app/WorkspaceFileManager.{h,m}`
- `app/WorkspaceMarkdownViewer.{h,m}`
- `app/WorkspaceImageViewer.{h,m}`
- `app/WorkspaceVideoPlayer.{h,m}`
- `app/MarkdownRenderer.{h,m}` (factored out of AboutViewController.m)

WorkspaceViewController.m only gains the registration-table branches.

---

## Layer 1: File-open routing (how applets talk to each other)

A small registry, not a framework:

- Protocol `WorkspaceFileOpenable`:
  `- (void)workspaceOpenFileAtGuestPath:(NSString *)path;`
  adopted by the four viewers, MotePad, and the Music applet.
- Routing table `ISHWorkspaceToolIdentifierForFileExtension()` next to the
  other ISHWorkspaceTool* helpers:
  - md, markdown -> markdown viewer
  - png, jpg, jpeg, gif, heic, heif, webp, bmp, tiff -> image viewer
  - mp4, mov, m4v -> video player (AVFoundation's actual container support)
  - mp3, m4a, aac, flac, ogg, opus, wav -> Music (`audio` identifier;
    matches the decoder set in AudioPCMDecoder_*)
  - svg, html, htm -> Browser applet (WKWebView renders these natively;
    UIImage cannot do SVG)
  - everything else that stats as a regular file and looks textual
    (heuristic: no NUL in first 8 KiB) -> MotePad; otherwise no default
    handler and the row's Open action is replaced by Open With / Get Info.
- Workspace controller entry point:
  `- (void)openWorkspaceToolWithIdentifier:(NSString *)identifier
                              fileGuestPath:(NSString *)path;`
  Creates or reuses the applet window via the existing
  `openWorkspaceToolWindowWithIdentifier:` path, then delivers the path via
  the protocol after `attachViewController:` completes. Reuse policy:
  viewers reuse an existing window on the current desktop by default
  (Finder/Preview behavior); the file manager itself is multi-window.

The Music applet's adoption is small: `workspaceOpenFileAtGuestPath:` calls
`ISHAudioLibrary resolvePlayableURLForGuestPath:` (already exists,
AudioLibrary.m:219), inserts the track as now-playing, and starts playback.
A directory dropped on Music (or "Add Folder to Music" in the file manager
context menu) routes to `scanGuestDirectory:` to add to the library.

---

## Layer 2: The file manager applet

Identifier `files`, class `WorkspaceFileManagerToolViewController`
(WorkspaceThemedToolViewController subclass), title "Files"... except that
collides with iOS Files.app naming in users' heads; suggest title "Finder"
is too cheeky, so: **"File Manager"** in the dock menu, window title shows the
current folder name (Finder behavior).

### Layout (modeled on a Finder window)

- Left: collapsible sidebar (UICollectionView list layout, sidebar
  appearance). Sections:
  - Favorites: user-editable, persisted in NSUserDefaults. Seed with the
    guest home (/root), /AOK/persist, /tmp, /AOK.
  - Locations: `/` plus mounted roots (source the same list the Filesystems
    applet / Roots.m uses), /proc and /dev included but marked virtual.
  - Sidebar hides automatically below ~520 pt window width (the window
    container reports size changes via frameDidChangeHandler).
- Top toolbar strip inside the content view: back / forward (history stack
  per window), up, view-mode toggle (list | icons), sort menu, new-folder,
  and an overflow menu (show hidden files, refresh).
- Main pane: UICollectionView.
  - List view (v1): compositional list layout, UIListContentConfiguration:
    SF Symbol icon, name, size, mtime. Swipe actions for delete/rename.
  - Icon grid view (phase 4): flow of icon+label cells, image thumbnails.
  - Column/Miller view: explicitly deferred (high effort, low payoff in a
    360-700 pt window); revisit after v1 feedback.
- Bottom: breadcrumb path bar, each component tappable (Finder's path bar),
  plus an item-count / selection-count / free-space status line (statfs via
  the bridge; tmpfs statfs already reports real numbers per 9e0388c2).

### Interactions (native UIKit only, per standing feedback)

- Tap = select, double-tap = open (routes through Layer 1); directories
  navigate in place.
- UIContextMenuInteraction menu per item: Open, Open With > (submenu of all
  capable applets + MotePad + "New Terminal Here" for directories), Get
  Info, Rename, Duplicate, Copy, Move ("Cut" semantics via a paste-board
  stack, Paste appears on folder/background menu), Compress: out of scope,
  Share (extractToTemp then UIActivityViewController; iPad popover MUST be
  anchored to the cell, standing rule), Delete.
- Multi-select via the standard collection-view editing mode (Select button
  in toolbar) rather than custom gestures.
- Drag and drop: UICollectionViewDragDelegate/DropDelegate. Within one
  window = move; between two file-manager windows = copy (Finder
  cross-volume behavior; long term modifier-aware, v1 fixed policy). Items
  carry the guest path in a local NSItemProvider; drops from outside the app
  are out of scope (FileProvider covers that direction).
- Hardware keyboard (iPad): UIKeyCommands for cmd-up (parent), cmd-down /
  return (open), cmd-delete (delete), cmd-N (new file-manager window),
  cmd-shift-. (toggle hidden), space (open in the routed viewer, our
  QuickLook stand-in).
- Get Info: anchored popover with full stat detail (kind, size, mtime,
  mode string, uid/gid, symlink target, guest path). Read-only in v1;
  chmod/chown editing in phase 4.
- Delete: confirmation alert, then unlink/rmdirRecursive. Finder-style
  Trash (move to ~/.Trash in the guest) is a phase-4 option, default off;
  shell users expect rm semantics and a trash surprises them.

### Behavior details

- Directory listing is async with a spinner; a listing that exceeds ~5k
  entries renders incrementally (diffable data source snapshots in chunks).
- Sort: name (default, dirs first), size, mtime, kind. Persisted per applet,
  not per folder (v1).
- Hidden files (dotfiles) hidden by default.
- Symlinks: shown with the badge arrow, single stat of the target for
  kind/size display; opening follows the target; broken links get the
  broken badge and only Get Info / Delete.
- Refresh: manual button + automatic re-list when the window becomes
  frontmost (didBecomeFrontmostHandler). No inotify equivalent exists in the
  bridge; polling every open window is not worth it in v1.
- Multi-instance: NOT in `isGlobalToolIdentifier:`; every open request makes
  a new window so two windows can drag between each other. Layout
  persistence already handles multiple tool windows with the same
  identifier. Per-window state (cwd, history) persists via the saved-layout
  descriptor; add an optional `state` dict to the `tool` descriptor in
  `savedLayoutDescriptorForWindow:` (WorkspaceViewController.m:3054) that
  round-trips through the WorkspaceFileOpenable applets. This is the one
  small change to the persistence layer and benefits every applet (viewers
  can restore their last file the same way).

---

## Layer 3: Viewer applets

All three follow the same skeleton: WorkspaceThemedToolViewController
subclass, empty state ("no file open" + an Open... button that presents a
minimal folder picker reusing the file-manager browsing view), adopts
WorkspaceFileOpenable, reloads on demand, and shows a plain error card for
bridge failures (errno text from NSError+ISHErrno).

### 3a. Markdown reader (identifier `markdown`)

- First, factor the existing dependency-free renderer
  (`ISHLLMAttributedStringFromMarkdown` and friends,
  AboutViewController.m:1016-1231) into `app/MarkdownRenderer.{h,m}` and
  point the LLM chat at it. One renderer, two clients; divergence is the
  thing this step prevents.
- View: non-editable UITextView (selectable for copy) with the rendered
  attributed string; re-render on ISHWorkspaceToolThemeDidChangeNotification
  so code blocks/quotes track the theme.
- Size cap: read via `readFile(path, 4 MiB)`; bigger files get a truncation
  banner (real markdown that size is a pathology).
- Links: http(s) taps route to the Browser applet; relative .md links
  resolve against the current file's directory and navigate in-reader with
  back/forward history; relative image references render inline via bridge
  reads (downsampled, phase-2 stretch, plain text placeholder first).
- Renderer upgrades needed for reader duty (the LLM chat never needed
  them): tables (pipe syntax) and inline images. Add to MarkdownRenderer
  behind options so chat behavior is unchanged.
- Toolbar: Open in MotePad (edit round-trip), Reload.

### 3b. Image viewer (identifier `imageviewer`)

- UIScrollView + UIImageView; pinch zoom, double-tap toggling fit/100%,
  min zoom = fit, max = max(4x, 100%).
- Loading: `readFile` capped at 64 MiB; decode via ImageIO with
  `CGImageSourceCreateThumbnailAtIndex` downsampling to a bounded pixel
  budget (~2x window size in pixels, re-decode on significant zoom-in).
  Full-res UIImage init on a 100 MP photo is a jetsam event; downsampling
  is mandatory, not an optimization.
- Animated GIF/WebP: decode frames via ImageIO into an animated UIImage.
- Prev/next arrows (and left/right key commands) walk sibling images in the
  same directory sorted by name: Preview.app behavior, cheap to add since
  the open path already listed the directory in the file manager; the
  viewer re-lists on demand via the bridge.
- Toolbar: fit / actual size, share (extractToTemp + activity controller,
  anchored popover), Reload. Title = filename + pixel dimensions.

### 3c. Video player (identifier `videoplayer`)

- AVPlayerViewController embedded as a child VC (first AVPlayer use in the
  app; nothing to conflict with). Gets scrubber, rate control, AirPlay,
  fullscreen for free. Fullscreen presents from the workspace controller.
- Source resolution, in order:
  1. Realfs-backed path (item.isRealfsBacked): play the host URL directly.
     Zero copy; this is the common case for media users keep in
     /AOK/persist.
  2. Fakefs/tmpfs path, v1: `extractToTemp` with a determinate progress
     overlay and cancel. LRU temp cache means replay is instant.
  3. Phase-3b upgrade: AVAssetResourceLoaderDelegate with a custom URL
     scheme (`ishguest://`) answering byte-range requests via the bridge's
     `pread` handle. True streaming: no extraction latency, no double disk.
     mp4 needs random access (moov atom often at the end), which pread
     gives us. Ship extraction first; the loader is contained and testable
     after.
- Formats: whatever AVFoundation decodes (H.264/HEVC in mp4/mov/m4v, AAC).
  mkv/webm/avi get a clear "container not supported by the iOS media stack"
  error, not a spinner. No ffmpeg; standing implement-don't-stub applies to
  syscalls, not to relitigating iOS codec licensing.
- Audio coexistence: on play, pause ISHAudioLibrary playback (the custom
  PCM engine and AVPlayer will otherwise mix or fight over the audio
  session). Configure AVAudioSession playback category to match what
  AudioOutput.m already sets; verify no session-category churn on
  open/close.

---

## Wiring checklist (per applet, all in WorkspaceViewController.m)

Mechanical, same seven touchpoints every time:
1. Identifier constant near line 99.
2. Preferred/min/max content sizes (`ISHWorkspacePreferredToolContentSize`
   :867, Minimum :989, Maximum :~1056). Suggested preferred: file manager
   720x480, markdown 560x600, image 640x480, video 640x400.
3. Title in `ISHWorkspaceToolTitle` (:1084).
4. Factory branch in `ISHCreateWorkspaceToolViewController` (:2403).
5. Reverse map in `ISHWorkspaceToolIdentifierForViewController` (:2448).
6. Dock menu entry in `dockUtilityGroupDescriptors` (:5405); file manager
   under Storage group, viewers under a new "Media" group.
7. Launcher command words (:271): "files", "fm" -> files; "md" -> markdown;
   "img" -> imageviewer; "video" -> videoplayer.

Window persistence, theming, drag/resize/zoom chrome all come free.

---

## Phasing

Phase 0: GuestFileBridge + NSError+ISHErrno promotion + MarkdownRenderer
  extraction (pure refactor, LLM chat must be pixel-identical after).
  Exit: a throwaway debug applet can list /, read a file from fakefs and
  from /AOK/persist, and extract a 100 MB file with progress + cancel.

Phase 1: File manager v1: sidebar, list view, navigation/history/breadcrumb,
  context menu (Open/Rename/Duplicate/Copy-Paste/Delete/Get Info/New
  Folder), multi-select, sort, hidden toggle, open routing to MotePad only.
  Exit: full manual matrix below passes on device.

Phase 2: Markdown reader + image viewer + routing table + "space to
  preview". Exit: browse a Devuan root's /usr/share/doc comfortably; photo
  directory in /AOK/persist/pictures browses with prev/next.

Phase 3: Video player (realfs direct + extraction path) + Music applet
  integration (open + Add Folder to Music). 3b: resource-loader streaming
  for fakefs. Exit: mp4 in fakefs plays with scrub; mp3 opens into Music
  playing.

Phase 4: Finder polish: icon grid with image thumbnails (bridge reads on a
  second borrow-context queue, disk thumbnail cache), drag and drop between
  file-manager windows, Get Info chmod editing, optional Trash, statfs
  status bar if not already in v1.

Phase 5: Dedupe: AudioLibrary + MotePadDocumentStore migrate onto
  GuestFileBridge; delete both private copies of withGuestTaskContext.

---

## Test plan

No CLI-harness coverage is possible (UIKit), so this is a device matrix plus
one guarded invariant: GuestFileBridge asserts it is never entered on the
main thread.

Manual matrix (iPad M4 rig + iPhone-size window widths):
- Backends: fakefs root (devuan amd64), /AOK/persist (realfs), /tmp (tmpfs),
  /proc and /dev (virtual, read-only expectations).
- Content: 10k-entry directory (incremental render, no beachball), unicode
  and emoji filenames, deep paths, symlink chains + broken links + a
  symlink loop, permission-denied directory (clean errno surface),
  FIFO/socket/device nodes in /dev (must not block or open).
- Lifecycle: guest not booted yet (empty states everywhere), file deleted
  from a shell while open in a viewer (reload shows errno, no crash),
  directory deleted under an open file-manager window, applet window closed
  mid-extraction (cancel fires, temp partial removed), app background/
  foreground during extraction.
- Media: 4 GB mp4 extraction progress + cancel + cache hit on second open;
  video and music started together (music pauses); 100 MP jpeg (downsample,
  no jetsam); animated gif.
- Persistence: two file-manager windows on different desktops with different
  cwds survive app restart; viewers restore last file when it still exists
  and show the empty state when it does not.
- Regression: MotePad and Music behave identically post-phase-5 migration;
  LLM chat rendering identical post-MarkdownRenderer extraction.

---

## Risks / open questions

- WorkspaceViewController.m growth: mitigated by the separate-file rule; the
  factory-table pattern makes that painless.
- fakefs lock hold times: long extractions run chunked on the serial ioQueue,
  so a 4 GB extraction serializes behind-the-scenes bridge users (e.g. a
  directory listing) for its duration. Mitigation: chunked ops re-enqueue
  between chunks so queued listings interleave; if that is not enough, a
  second borrow-context queue for bulk transfers.
- Two windows editing the same tree have no change notification; v1 accepts
  refresh-on-focus. A bridge-level "paths changed" NSNotification posted
  after every mutating op fixes the intra-app case cheaply and is worth
  doing in phase 1.
- iPad popovers: every sheet/activity controller MUST have a popover anchor
  (standing rule from the action-sheet crash).
- Video streaming (3b) correctness under seek-heavy use needs the pread
  handle to be robust against the guest file changing size mid-play; the
  (size, mtime) key invalidates the cache but a live AVAsset just gets EOF
  behavior, which is acceptable.
- Naming: `files` identifier vs. the Files.app FileProvider work; identifiers
  are internal so no real conflict, but keep dock title "File Manager".
