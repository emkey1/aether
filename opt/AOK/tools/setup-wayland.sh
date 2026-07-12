#!/bin/sh
# setup-wayland.sh
# ---------------------------------------------------------------------------
# Install a headless Wayland desktop (compositor + terminal + VNC bridge) into
# the current guest root, for the Workspace "Display" applet. Installs:
#   labwc    - wlroots-based Wayland compositor (headless backend, no
#              DRM/udev/seatd needed -- see start-wayland.sh)
#   foot     - lightweight Wayland terminal, the default first app
#   wayvnc   - VNC server for wlroots compositors; the applet's WebSocket
#              bridge connects to it over TCP
#
# Run as root:
#       sudo sh /AOK/tools/setup-wayland.sh
#   or  doas sh /AOK/tools/setup-wayland.sh
#
# Devuan (apt) is supported now; Alpine (apk) is a documented follow-up (the
# packages exist in edge/community but haven't been bring-up-tested the way
# the Devuan path has -- see wayland_workspace_plan.md phase 0).
#
# Only amd64/x86_64 has been bring-up-tested (cage/labwc + wlroots' headless
# backend + wayvnc, verified both in the CLI harness and on-device). Other
# guest arches may work -- the packages exist for them in Devuan -- but
# haven't been verified, so this script warns rather than refuses.
# ---------------------------------------------------------------------------
set -u

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'setup-wayland.sh: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root:  sudo sh $0"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) : ;;
    *) note "warning: guest arch '$ARCH' is untested for the Wayland stack;"
       note "proceeding anyway -- report back what breaks." ;;
esac

if command -v apt-get >/dev/null 2>&1; then
    log "Devuan/Debian (apt) detected"

    # deb.devuan.org's round-robin can land on a slow/unreliable mirror (seen:
    # <1KB/s, dropped connections mid-transfer); pin the master mirror instead
    # of gambling on the pool, matching the fix used during Wayland bring-up.
    SOURCES=/etc/apt/sources.list
    if [ -f "$SOURCES" ] && grep -q 'deb\.devuan\.org' "$SOURCES" 2>/dev/null; then
        note "pinning pkgmaster.devuan.org (deb.devuan.org's round-robin can be very slow)"
        sed -i 's/deb\.devuan\.org/pkgmaster.devuan.org/g' "$SOURCES"
    fi

    log "apt-get update"
    apt-get update || die "apt-get update failed -- check network/DNS (guest /etc/resolv.conf)"

    log "installing labwc, foot, wayvnc"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        labwc foot wayvnc \
        || die "apt-get install failed -- see output above"

elif command -v apk >/dev/null 2>&1; then
    log "Alpine (apk) detected"
    note "warning: the Alpine path is untested -- please report back what breaks."
    apk add labwc foot wayvnc || die "apk add failed -- see output above"

else
    die "no supported package manager found (need apt-get or apk)"
fi

for bin in labwc foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 || die "install reported success but '$bin' is not on PATH"
done

log "done"
note "labwc, foot, and wayvnc are installed."
note "The Display applet will launch the stack automatically from here on."
