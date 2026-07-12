# noVNC (vendored subset)

Source: https://github.com/novnc/noVNC, tag `v1.6.0`.

Only `core/` (the RFB protocol/decoder/input library, MPL-2.0) and
`vendor/pako/` (zlib decompression, MIT) are vendored here — the rest of the
upstream tree (the full `vnc.html` app UI, tests, build tooling, fonts) isn't
needed since `app/display/display.html` is our own minimal viewer in the
style of upstream's `vnc_lite.html` example.

Unmodified from upstream except this file. See `LICENSE.txt` and `AUTHORS`
for full license text and attribution; `vendor/pako/LICENSE` for pako's MIT
license.

To update: replace `core/` and `vendor/pako/` with a newer tagged release,
diff `app/display/display.html`'s API usage (`RFB` constructor options,
event names) against upstream's `vnc_lite.html` for that tag before bumping.
