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
#                      On any die() below, "$ISH_DISPLAY_READY_FILE.error" is
#                      written instead with the failure reason, so
#                      DisplayViewController's poll can surface the actual
#                      cause (labwc/foot/wayvnc crash, timeout, etc.)
#                      immediately instead of only ever reporting its own
#                      generic "timed out waiting" after the full deadline.
#
# Fixed for v1 (matches wayland_workspace_plan.md phase 2.5): the headless
# output is wlroots' default size (1280x720); the applet's native RFB client
# (DisplayRFBClient/DisplayRFBView) scales to fit. Per-session resizable
# output is a follow-up, not built here.
#
# First-run only, a minimal labwc menu.xml + a pipemenu script get seeded
# under $HOME/.config/labwc/ (see below) so right-click gives you more than
# labwc's compiled-in "Reconfigure"/"Exit" fallback -- a "New Terminal" entry
# and an "Applications" submenu that lists whatever's apt-installed.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
die()  {
    printf 'start-wayland.sh: %s\n' "$*" >&2
    printf '%s\n' "$*" > "$ERROR_FILE" 2>/dev/null
    exit 1
}

WAYVNC_PORT="${WAYVNC_PORT:-5901}"
COMPOSITOR_CMD="${WAYLAND_COMPOSITOR_CMD:-labwc}"
READY_FILE="${ISH_DISPLAY_READY_FILE:-/tmp/ish-display.ready}"
ERROR_FILE="$READY_FILE.error"
rm -f "$READY_FILE" "$ERROR_FILE"

for bin in $COMPOSITOR_CMD foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 \
        || die "'$bin' not found -- run 'sudo sh /AOK/tools/setup-wayland.sh' first"
done

# Defensively clean up any stale labwc/foot/wayvnc left over from a prior
# session that didn't get torn down before this one started (a fast
# Reconnect, or closing and reopening the applet, can both race ahead of the
# old session's async guest-side cleanup -- the native side now waits for
# confirmed exit before reconnecting, but this covers it independent of
# that, since every session binds the same fixed WAYVNC_PORT regardless of
# who started it). Without this, overlapping instances fight over that port
# and pile up as unreaped zombies that drag the whole guest's scheduling
# down until the applet looks wedged.
pkill -x "$COMPOSITOR_CMD" 2>/dev/null
pkill -x foot 2>/dev/null
pkill -x wayvnc 2>/dev/null
sleep 0.2

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
# There's no real GPU/DRM device here (matches labwc's own harmless
# "drmGetDevices2 failed: No such file or directory" at startup). GTK3
# apps generally cope via cairo software rendering by default, but GTK4's
# GskRenderer tries GL/Vulkan first -- confirmed on-device: gnome-calculator
# hit "MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_
# DRIVER)" and only rendered once forced to cairo/software. Exported here
# (not just set for wayvnc/foot individually) so every client an interactive
# session launches -- typically from a shell inside foot -- inherits them
# without the user needing to know this environment has no GPU.
export GSK_RENDERER=cairo
export LIBGL_ALWAYS_SOFTWARE=1

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

# Every subprocess's stdout/stderr also goes to this file (in addition to
# >&2, the pty DisplayViewController owns, which nothing else can tail) --
# without it, a crash right after launch (e.g. wayvnc/foot dying before
# their first line of output reaches the pty) leaves no way to diagnose
# *why* short of re-running this script by hand over SSH.
DEBUG_LOG="${ISH_DISPLAY_DEBUG_LOG:-/tmp/ish-wayland-debug.log}"
: > "$DEBUG_LOG"

# Seed a minimal-but-useful labwc config. Without this, labwc falls back to
# its compiled-in root menu, which is just "Reconfigure"/"Exit" and gives no
# way to launch anything beyond the one foot window this script starts. The
# "Applications" entry is a labwc pipemenu -- labwc runs list-apps.sh and
# expects an <openbox_pipe_menu> XML fragment on stdout, regenerated every
# time that submenu opens -- so whatever gets apt-installed later shows up
# automatically, no menu.xml edits needed.
if [ "$COMPOSITOR_CMD" = "labwc" ]; then
    mkdir -p "$HOME/.config/labwc"
    # Regenerated on every session start (not gated on "doesn't already
    # exist" like menu.xml below) -- it's a generated helper, not something
    # a user would hand-edit, and it needs to actually pick up fixes. A
    # single awk pass per .desktop file replaces what used to be ~9 forked
    # subprocesses per file (grep/cut/sed chained several times over) --
    # spawning that many processes per file was slow enough on its own to
    # make the menu look empty/unresponsive (labwc giving up on a pipemenu
    # that takes too long), and the resulting burst of rapid fork/exit/
    # SIGCHLD activity is also the likely trigger for a separate lock-
    # contention issue observed in the emulator's own signal/poll code when
    # this menu was opened -- worth its own investigation, but cutting the
    # process count this much should avoid triggering it in the first place.
    cat > "$HOME/.config/labwc/list-apps.sh" <<'LIST_APPS_EOF'
#!/bin/sh
echo '<openbox_pipe_menu>'
for f in /usr/share/applications/*.desktop; do
    [ -f "$f" ] || continue
    awk '
        /^NoDisplay=true/ { hidden=1 }
        /^Hidden=true/ { hidden=1 }
        /^Name=/ && name == "" { name = substr($0, 6) }
        /^Exec=/ && execline == "" { execline = substr($0, 6) }
        END {
            if (hidden || name == "" || execline == "") exit
            gsub(/%[a-zA-Z]/, "", execline)
            gsub(/&/, "\\&amp;", name); gsub(/</, "\\&lt;", name); gsub(/>/, "\\&gt;", name)
            gsub(/&/, "\\&amp;", execline); gsub(/</, "\\&lt;", execline); gsub(/>/, "\\&gt;", execline)
            printf "<item label=\"%s\"><action name=\"Execute\"><command>%s</command></action></item>\n", name, execline
        }
    ' "$f"
done
echo '</openbox_pipe_menu>'
LIST_APPS_EOF
    chmod +x "$HOME/.config/labwc/list-apps.sh"
fi
if [ "$COMPOSITOR_CMD" = "labwc" ] && [ ! -f "$HOME/.config/labwc/menu.xml" ]; then
    cat > "$HOME/.config/labwc/menu.xml" <<MENU_EOF
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu>
  <menu id="root-menu" label="Root">
    <item label="New Terminal">
      <action name="Execute"><command>foot</command></action>
    </item>
    <separator/>
    <menu id="apps-pipemenu" label="Applications" execute="$HOME/.config/labwc/list-apps.sh"/>
    <separator/>
    <item label="Reconfigure">
      <action name="Reconfigure"/>
    </item>
    <item label="Exit">
      <action name="Exit"/>
    </item>
  </menu>
</openbox_menu>
MENU_EOF
fi

log "starting $COMPOSITOR_CMD (headless)"
$COMPOSITOR_CMD 2>&1 | tee -a "$DEBUG_LOG" >&2 &
COMPOSITOR_PID=$!

# Wait for the compositor to create its Wayland socket rather than a fixed
# sleep -- labwc under JIT on a loaded device can take longer than the ~1s
# it takes on a native host.
i=0
while [ ! -S "$XDG_RUNTIME_DIR/wayland-0" ] && [ $i -lt 100 ]; do
    kill -0 "$COMPOSITOR_PID" 2>/dev/null || die "$COMPOSITOR_CMD exited before creating its Wayland socket -- see $DEBUG_LOG"
    sleep 0.1
    i=$((i + 1))
done
[ -S "$XDG_RUNTIME_DIR/wayland-0" ] || die "$COMPOSITOR_CMD did not create wayland-0 within 10s"

# The socket *file* existing doesn't mean labwc's event loop is actually
# accepting Wayland client connections yet -- observed on-device: wayvnc
# started immediately after the socket appeared and still hit "failed to
# connect to wayland; no compositor running?" (from wayvnc's own log), i.e.
# a real connect()-level race, not just a slow client. A short grace period
# here is cheaper and more robust than trying to detect "actually ready"
# any more precisely.
sleep 0.3

# wayvnc gets its own retry loop on top of the grace sleep above: the same
# labwc-not-quite-ready race can still occasionally lose even with the
# sleep (JIT-emulation timing is not consistent run to run), and wayvnc
# fails fast (connection refused) rather than hanging, so a few cheap
# retries turn an intermittent failure into a reliable success instead of a
# full die()/Reconnect round trip. foot is deliberately started only AFTER
# this loop confirms wayvnc is actually listening -- that's empirical proof
# labwc is truly ready, which sidesteps the same race for foot too rather
# than needing a second, duplicated retry loop (found on-device: giving
# only wayvnc a retry loop while starting foot at the same fixed point as
# before just moved the race onto foot -- it died with the identical
# "no compositor running" race on its one and only attempt).
wayvnc_is_listening() {
    kill -0 "$WAYVNC_PID" 2>/dev/null \
        && awk -v port="$hex_port" '$2 ~ (":" port "$") && $4 == "0A" { found=1 } END { exit !found }' /proc/net/tcp 2>/dev/null
}

hex_port=$(printf '%04X' "$WAYVNC_PORT")
wayvnc_attempt=1
while true; do
    log "starting wayvnc on :$WAYVNC_PORT (attempt $wayvnc_attempt)"
    WAYLAND_DISPLAY=wayland-0 wayvnc 127.0.0.1 "$WAYVNC_PORT" 2>&1 | tee -a "$DEBUG_LOG" >&2 &
    WAYVNC_PID=$!

    # Confirm wayvnc is both still alive AND actually bound/listening before
    # declaring ready -- checking liveness alone races wayvnc's own startup:
    # `kill -0` succeeds the instant fork() returns, long before wayvnc has
    # opened its listening socket. /proc/net/tcp is checked directly (no
    # `ss`/`netstat` dependency) for a LISTEN (0A) entry on WAYVNC_PORT.
    i=0
    wayvnc_listening=0
    while [ $i -lt 150 ]; do
        wayvnc_is_listening && { wayvnc_listening=1; break; }
        kill -0 "$WAYVNC_PID" 2>/dev/null || break
        i=$((i + 1))
        sleep 0.1
    done

    if [ "$wayvnc_listening" = 1 ]; then
        # A single instantaneous LISTEN read isn't enough: the same
        # labwc-not-quite-ready race can let wayvnc bind its VNC socket
        # first, then fail its *internal* reconnect to WAYLAND_DISPLAY a
        # moment later and drop back off LISTEN while it retries that --
        # observed on-device: the applet's bridge got ECONNREFUSED
        # connecting seconds after this exact check had already passed.
        # Recheck after a short settle window instead of trusting one
        # instantaneous read.
        sleep 1.0
        wayvnc_is_listening && break
        wayvnc_listening=0
    fi

    [ "$wayvnc_attempt" -ge 4 ] && die "wayvnc never reached a stable listening state on :$WAYVNC_PORT after $wayvnc_attempt attempts -- see $DEBUG_LOG"
    kill "$WAYVNC_PID" 2>/dev/null
    wayvnc_attempt=$((wayvnc_attempt + 1))
    sleep 0.3
done
log "starting foot"
WAYLAND_DISPLAY=wayland-0 foot 2>&1 | tee -a "$DEBUG_LOG" >&2 &
FOOT_PID=$!
# foot isn't load-bearing for the applet's own readiness (wayvnc is what the
# bridge connects to), but a foot that dies immediately even at this
# already-proven-ready point is a strong signal something is wrong with the
# whole compositor session -- fail loudly instead of declaring ready into
# an empty desktop. A brief settle sleep before checking, since `kill -0`
# immediately after fork() would just prove foot forked, not that it's
# still alive moments later.
sleep 0.2
kill -0 "$FOOT_PID" 2>/dev/null || die "foot exited immediately after starting -- see $DEBUG_LOG"

echo "READY $WAYVNC_PORT"
printf '%s\n' "$WAYVNC_PORT" > "$READY_FILE"

wait "$COMPOSITOR_PID" "$FOOT_PID" "$WAYVNC_PID"
