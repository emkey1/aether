#!/bin/sh
# setup-wayland.sh
# ---------------------------------------------------------------------------
# Install a headless Wayland desktop (compositor + terminal + VNC bridge) into
# the current guest root, for the Workspace "Display" applet. Installs:
#   labwc    - wlroots-based Wayland compositor (headless backend, no
#              DRM/udev/seatd needed -- see start-wayland.sh); the default
#              compositor -- floating/stacking windows, right-click menu,
#              themed to match the app's own Workspace look
#   sway     - a second wlroots-based compositor, kept installed as a
#              WAYLAND_COMPOSITOR_CMD=sway alternative (tiling by default,
#              keybinding-driven instead of a menu)
#   wofi     - dmenu-style app launcher, invoked by sway's Alt+d binding
#              (only relevant if you switch to sway)
#   foot     - lightweight Wayland terminal, the default first app
#   wayvnc   - VNC server for wlroots compositors; the applet's native RFB
#              client connects to it over TCP
# Also installs (best-effort, won't block the above): htop, btop, mc,
# neovim, vim -- so the Display applet's Applications menu (list-apps.sh,
# see start-wayland.sh) isn't nearly empty on a fresh rootfs. That menu just
# reflects whatever's already installed with a .desktop file; with nothing
# beyond the required packages above it would only ever show foot's own
# three entries.
#
# Run as root:
#       sudo sh /AOK/tools/setup-wayland.sh
#   or  doas sh /AOK/tools/setup-wayland.sh
#
# Devuan (apt) is supported now; Arch (pacman) installs the same stack (all
# ten packages carry identical names in the Arch repos); Alpine (apk) is a
# documented follow-up (the packages exist in edge/community but haven't
# been bring-up-tested the way the Devuan path has -- see
# wayland_workspace_plan.md phase 0).
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

    log "installing labwc, sway, wofi, foot, wayvnc"
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        labwc sway wofi foot wayvnc \
        || die "apt-get install failed -- see output above"

elif command -v pacman >/dev/null 2>&1; then
    log "Arch (pacman) detected"
    # All five packages carry the same names in the Arch repos as in Devuan.
    # The Arch provisioner (provision-ultimate-archlinux.sh) has already
    # disabled pacman's Landlock sandbox and signature checking for iSH; if
    # this root wasn't provisioned by it, pacman -Sy may fail on those --
    # run the provisioner first in that case.
    log "pacman -Sy"
    pacman -Sy --noconfirm || die "pacman -Sy failed -- check network/DNS (guest /etc/resolv.conf)"
    log "installing labwc, sway, wofi, foot, wayvnc"
    pacman -S --needed --noconfirm labwc sway wofi foot wayvnc \
        || die "pacman -S failed -- see output above"

elif command -v apk >/dev/null 2>&1; then
    log "Alpine (apk) detected"
    note "warning: the Alpine path is untested -- please report back what breaks."
    apk add labwc sway wofi foot wayvnc || die "apk add failed -- see output above"

else
    die "no supported package manager found (need apt-get, pacman, or apk)"
fi

for bin in labwc sway wofi foot wayvnc; do
    command -v "$bin" >/dev/null 2>&1 || die "install reported success but '$bin' is not on PATH"
done

# Best-effort, not required: a renamed/missing package on some future
# Debian/Alpine release shouldn't block installing the actual Wayland stack
# above, which just finished and is already verified working.
MENU_APPS="htop btop mc neovim vim"
log "installing $MENU_APPS for the default Applications menu (best-effort)"
if command -v apt-get >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $MENU_APPS \
        || note "warning: some of these failed to install -- Applications menu will just show whichever succeeded"
elif command -v pacman >/dev/null 2>&1; then
    pacman -S --needed --noconfirm $MENU_APPS \
        || note "warning: some of these failed to install -- Applications menu will just show whichever succeeded"
elif command -v apk >/dev/null 2>&1; then
    apk add $MENU_APPS \
        || note "warning: some of these failed to install -- Applications menu will just show whichever succeeded"
fi

log "done"
note "labwc, sway, wofi, foot, and wayvnc are installed."
note "The Display applet will launch the stack automatically from here on."
