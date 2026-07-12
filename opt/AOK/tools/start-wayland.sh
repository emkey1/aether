#!/bin/sh
# start-wayland.sh
# ---------------------------------------------------------------------------
# Launch the headless Wayland desktop for the Workspace "Display" applet:
# labwc (compositor) + foot (first terminal) + wayvnc (VNC bridge target).
# Runs in the foreground as the session leader of a pseudo-terminal owned by
# DisplayViewController (the same pty-backed session mechanism
# TerminalViewController uses, minus the visible terminal UI); closing the
# applet hangs up that pty, which delivers SIGHUP here. TERM/INT are also
# trapped for when something signals this script directly (e.g. testing it
# by hand from a shell). Requires labwc, foot, and wayvnc on PATH -- run
# /AOK/tools/setup-wayland.sh once first if they're missing.
#
# Env overrides (all optional):
#   WAYVNC_PORT       TCP port wayvnc listens on (default 5901; 5900 is
#                      commonly taken by other VNC/screen-sharing services)
#   WAYLAND_COMPOSITOR_CMD
#                      compositor invocation (default: "labwc"). cage is
#                      NOT supported here -- its virtual-keyboard keycodes
#                      are off by evdev's +8 offset and foot silently drops
#                      every key (see wayland_workspace_plan.md phase 0);
#                      only labwc has been verified.
#   ISH_DISPLAY_READY_FILE
#                      path this script writes "$WAYVNC_PORT\n" to once
#                      wayvnc is confirmed listening (default
#                      /tmp/ish-display.ready). DisplayViewController polls
#                      for this rather than trying to parse this script's
#                      pty-carried stdout, which goes through the terminal
#                      emulator's escape-sequence parser, not a plain pipe.
#                      Removed on exit and (defensively) at startup, in case
#                      a previous run was killed hard enough to skip cleanup.
#
# Fixed for v1 (matches wayland_workspace_plan.md phase 2.5): the headless
# output is wlroots' default size (1280x720); the applet's noVNC client
# scales to fit. Per-session resizable output is a follow-up, not built here.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
die()  { printf 'start-wayland.sh: %s\n' "$*" >&2; exit 1; }

WAYVNC_PORT="${WAYVNC_PORT:-5901}"
COMPOSITOR_CMD="${WAYLAND_COMPOSITOR_CMD:-labwc}"
READY_FILE="${ISH_DISPLAY_READY_FILE:-/tmp/ish-display.ready}"
rm -f "$READY_FILE"

for bin in $COMPOSITOR_CMD foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 \
        || die "'$bin' not found -- run 'sudo sh /AOK/tools/setup-wayland.sh' first"
done

# A runtime dir scoped to this invocation (pid-suffixed) avoids colliding
# with a leftover socket/lock from a prior run that didn't get torn down
# cleanly (observed during bring-up: a stale wayvnc control socket makes the
# next wayvnc refuse to start with "Another wayvnc process is already
# running").
export XDG_RUNTIME_DIR="/tmp/xdg-display-$$"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export HOME="${HOME:-/root}"
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1

COMPOSITOR_PID=""
FOOT_PID=""
WAYVNC_PID=""

cleanup() {
    trap - TERM INT HUP EXIT
    rm -f "$READY_FILE"
    [ -n "$WAYVNC_PID" ] && kill "$WAYVNC_PID" 2>/dev/null
    [ -n "$FOOT_PID" ] && kill "$FOOT_PID" 2>/dev/null
    [ -n "$COMPOSITOR_PID" ] && kill "$COMPOSITOR_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$XDG_RUNTIME_DIR"
    exit 0
}
trap cleanup TERM INT HUP EXIT

log "starting $COMPOSITOR_CMD (headless)"
$COMPOSITOR_CMD >&2 &
COMPOSITOR_PID=$!

# Wait for the compositor to create its Wayland socket rather than a fixed
# sleep -- labwc under JIT on a loaded device can take longer than the ~1s
# it takes on a native host.
i=0
while [ ! -S "$XDG_RUNTIME_DIR/wayland-0" ] && [ $i -lt 100 ]; do
    kill -0 "$COMPOSITOR_PID" 2>/dev/null || die "$COMPOSITOR_CMD exited before creating its Wayland socket"
    sleep 0.1
    i=$((i + 1))
done
[ -S "$XDG_RUNTIME_DIR/wayland-0" ] || die "$COMPOSITOR_CMD did not create wayland-0 within 10s"

log "starting foot"
WAYLAND_DISPLAY=wayland-0 foot >&2 &
FOOT_PID=$!

log "starting wayvnc on :$WAYVNC_PORT"
WAYLAND_DISPLAY=wayland-0 wayvnc 127.0.0.1 "$WAYVNC_PORT" >&2 &
WAYVNC_PID=$!

# Confirm wayvnc actually bound before declaring ready, so the applet isn't
# racing a not-yet-listening port on its first connect attempt.
i=0
while ! kill -0 "$WAYVNC_PID" 2>/dev/null; do
    [ $i -ge 50 ] && die "wayvnc exited before starting -- see output above"
    i=$((i + 1))
done

echo "READY $WAYVNC_PORT"
printf '%s\n' "$WAYVNC_PORT" > "$READY_FILE"

wait "$COMPOSITOR_PID" "$FOOT_PID" "$WAYVNC_PID"
