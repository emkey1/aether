#!/bin/sh
# provision-ultimate-archlinux.sh
# ---------------------------------------------------------------------------
# Turn a fresh Arch Linux / Arch Linux ARM rootfs (x86_64 OR aarch64) running
# under iSH-AOK into a full-featured, terminal-only Linux system. This is the
# Arch twin of provision-ultimate-alpine.sh / provision-ultimate-devuan.sh --
# same end result, translated to pacman instead of apk/apt:
#   * generous "ultimate terminal" CLI tool set
#   * daemons started directly in the background (sshd, syslog-ng, chronyd,
#     crond) -- see the "Services" section below for why, and its limits
#   * US/Pacific timezone (configurable)
#   * chrony in iSH-aware monitoring mode (the guest clock is the host clock)
#   * shell niceties: bash login shells, colour prompt, MOTD, login summary,
#     fzf/dircolors integration, machine-id, periodic maintenance via cron
#   * a dependency-free Neovim starter config (OSC52 clipboard on nvim >= 0.10)
#
# It is IDEMPOTENT: safe to run repeatedly. Run as root:
#       sudo sh provision-ultimate-archlinux.sh
#   or  doas sh provision-ultimate-archlinux.sh
#
# When run on a terminal it PROMPTS for the timezone and the primary login
# (creating that user if it does not exist). Pre-set any tunable via the
# environment to skip its prompt / run non-interactively:
#       TZ_NAME=America/Los_Angeles    # timezone (else prompted)
#       TARGET_USER=mke                # primary login to set up (else prompted)
#       NEW_HOSTNAME=                  # hostname to set (else prompted)
#       SUDO_NOPASSWD=0                # 1 = passwordless %wheel sudo
#
# EXPERIMENTAL / not officially supported (see kBundledRootDisplayNameKey in
# app/Roots.m): Arch ships real util-linux login + PAM (unlike Alpine/
# Devuan's simplified login), no distro-specific patching, and -- unlike
# Alpine (OpenRC) and Devuan (sysvinit) -- no non-systemd init at all. See
# "Services" below.
# ---------------------------------------------------------------------------
set -u

# ---- must be root --------------------------------------------------------
if [ "$(id -u)" != 0 ]; then
    echo "This script must run as root:  sudo sh $0" >&2
    exit 1
fi

log()  { printf '\n\033[1;36m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }

# ---- config (env overrides; prompts interactively when run on a TTY) ------
NEW_HOSTNAME="${NEW_HOSTNAME:-}"
SUDO_NOPASSWD="${SUDO_NOPASSWD:-0}"

# Defaults offered at the prompts.
DEF_TZ="${TZ_NAME:-America/Los_Angeles}"
DEF_USER="${TARGET_USER:-${SUDO_USER:-}}"
if [ -z "$DEF_USER" ] || [ "$DEF_USER" = root ]; then
    DEF_USER="$(awk -F: '$3>=1000 && $3<2000 {print $1; exit}' /etc/passwd)"
fi
[ -n "$DEF_USER" ] || DEF_USER="aok"
DEF_HOSTNAME="$(cat /etc/hostname 2>/dev/null)"
[ -n "$DEF_HOSTNAME" ] && [ "$DEF_HOSTNAME" != localhost ] || DEF_HOSTNAME="archlinux-ish"

# ask <var> <prompt> <default>: keep an env-provided value; else prompt on a
# TTY; else use the default (so piped/ssh runs never block).
ask() {
    eval "_cur=\${$1:-}"
    [ -n "$_cur" ] && return
    if [ -t 0 ]; then
        printf '%s [%s]: ' "$2" "$3"
        read _a || _a=""
        [ -n "$_a" ] || _a="$3"
    else
        _a="$3"
    fi
    eval "$1=\$_a"
}
ask TZ_NAME     "Timezone (e.g. America/New_York, UTC)" "$DEF_TZ"
ask TARGET_USER "Primary login username to set up"      "$DEF_USER"
ask NEW_HOSTNAME "Hostname"                              "$DEF_HOSTNAME"

# Create the chosen login if it does not exist yet (fresh rootfs).
if [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ] && ! id "$TARGET_USER" >/dev/null 2>&1; then
    useradd -m -s /bin/bash "$TARGET_USER" 2>/dev/null
    note "created login '$TARGET_USER' (no password set; give it one with: passwd $TARGET_USER)"
fi
TARGET_HOME=""
if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    TARGET_HOME="$(awk -F: -v u="$TARGET_USER" '$1==u{print $6}' /etc/passwd)"
fi
note "timezone=$TZ_NAME  login=${TARGET_USER:-<none>}  hostname=${NEW_HOSTNAME:-<keep>}"

# ===========================================================================
log "Installing packages (this is the slow part under emulation)"
# ===========================================================================
# Arch/pacman equivalents of the Alpine/Devuan "ultimate terminal" set. Names
# that differ from Devuan's list: build-essential -> base-devel (a group),
# python3 -> python (Arch's "python" package IS Python 3), bind9-dnsutils ->
# bind (provides dig/nslookup), netcat-openbsd -> openbsd-netcat, fd/bat/eza
# keep their upstream names (no Debian-style fd-find/batcat renaming),
# ncurses-bin/ncurses-term -> ncurses. lazygit and yq are in the official
# repos, unlike Devuan; the whole list is otherwise arch-independent
# (aarch64 and x86_64 provision identically).
PKGS="
  bash bash-completion cmake
  coreutils findutils grep sed gawk diffutils util-linux
  procps-ng shadow file less
  openssh sudo
  syslog-ng
  chrony cronie logrotate
  tzdata ca-certificates openssl
  man-db man-pages
  curl wget rsync bind iproute2
  git strace base-devel gdb
  python python-pip
  vim neovim nano tmux
  htop btop ncdu lsof pv tree
  mc fzf ripgrep fd bat eza jq yq most
  w3m lynx nmap socat openbsd-netcat mtr
  tar unzip zip p7zip bzip2 gzip zstd xz
  fastfetch figlet ncurses lazygit
"
# pacman >= 7.0 sandboxes downloads/hooks via Landlock LSM + a dedicated
# unprivileged "alpm" user by default. iSH's kernel doesn't implement Landlock
# at all, so every sync/install spews "restricting filesystem access failed
# because Landlock is not supported by the kernel!" / "switching to sandbox
# user 'alpm' failed!" and pacman -Sy can fail to fetch some/all databases.
# DisableSandbox turns that mechanism off; it's an accepted, documented
# pacman.conf option for exactly this class of environment (containers/chroots
# without Landlock), not a security-relevant setting for a single-user iOS app
# sandbox that has no isolation to lose anyway.
if [ -f /etc/pacman.conf ] && ! grep -q '^DisableSandbox' /etc/pacman.conf; then
    sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
    note "disabled pacman's Landlock sandbox (unsupported by iSH's kernel)"
fi
# iSH-AOK writes /etc/resolv.conf asynchronously during boot (AppDelegate's
# scheduleDnsRefresh dispatches it off the boot path rather than blocking on
# it), so a script launched right after boot can race it and see DNS lookups
# fail ("Could not resolve host") even though resolv.conf shows up moments
# later. Give it a few seconds to appear before syncing.
_waited=0
while [ ! -s /etc/resolv.conf ] && [ "$_waited" -lt 8 ]; do
    sleep 1
    _waited=$((_waited + 1))
done
if [ ! -s /etc/resolv.conf ]; then
    note "warning: /etc/resolv.conf is still empty after ${_waited}s -- DNS may not be configured yet"
fi
if ! pacman -Sy --noconfirm; then
    note "pacman -Sy failed; retrying once after 3s (transient DNS?)"
    sleep 3
    pacman -Sy --noconfirm || note "pacman -Sy failed again (continuing with cached index)"
fi
# pacman aborts the WHOLE transaction if even one package is unresolvable, so
# try the batch first and fall back to installing package-by-package (skipping
# any that are genuinely unavailable in this repo set). A failed/aborted batch
# can leave /var/lib/pacman/db.lck behind; clear it before the retry loop, or
# every single package -- including ones that are actually fine -- fails with
# "unable to lock database", which looks identical to "not found" once stderr
# is discarded. Keep stderr visible in the retry loop for exactly that reason.
if pacman -S --needed --noconfirm $PKGS; then
    note "packages installed"
else
    note "batch install failed; retrying package-by-package (skipping unavailable)"
    [ -f /var/lib/pacman/db.lck ] && { rm -f /var/lib/pacman/db.lck; note "  cleared stale db.lck from the aborted batch"; }
    for p in $PKGS; do
        # Capture pacman's own exit code before piping its output through sed
        # for indentation -- `if pacman ... | sed ...` would check sed's exit
        # status instead (sed always succeeds), silently defeating this check.
        _out="$(pacman -S --needed --noconfirm "$p" 2>&1)"
        _rc=$?
        [ -n "$_out" ] && printf '%s\n' "$_out" | sed 's/^/  /'
        [ "$_rc" -eq 0 ] || note "  skipped: $p"
    done
fi
pacman -Scc --noconfirm >/dev/null 2>&1 || true

# ===========================================================================
log "Timezone -> $TZ_NAME"
# ===========================================================================
if [ -f "/usr/share/zoneinfo/$TZ_NAME" ]; then
    ln -sf "/usr/share/zoneinfo/$TZ_NAME" /etc/localtime
    echo "$TZ_NAME" > /etc/timezone
    note "$(date)"
else
    note "zoneinfo for '$TZ_NAME' not found; leaving clock as-is"
fi

# ===========================================================================
log "Locale -> C.UTF-8"
# ===========================================================================
# Arch's glibc (>=2.35) has C.UTF-8 built directly into libc, so -- like
# Alpine's musl and Devuan's glibc -- no locale-gen step is needed. Arch's own
# convention is /etc/locale.conf (read by pam_env); also set /etc/environment
# so it applies to every session, not just ones that source /etc/profile.d.
if ! grep -q '^LANG=' /etc/locale.conf 2>/dev/null; then
    printf 'LANG=C.UTF-8\n' >> /etc/locale.conf
fi
if ! grep -q '^LANG=' /etc/environment 2>/dev/null; then
    printf 'LANG=C.UTF-8\nLC_ALL=C.UTF-8\n' >> /etc/environment
fi
note "LANG=C.UTF-8 (via /etc/locale.conf, /etc/environment)"

# ===========================================================================
log "machine-id"
# ===========================================================================
if [ ! -s /etc/machine-id ]; then
    { openssl rand -hex 16 2>/dev/null || head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n'; } > /etc/machine-id
    chmod 0444 /etc/machine-id
fi
if [ -d /var/lib/dbus ] && [ ! -e /var/lib/dbus/machine-id ]; then
    ln -sf /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true
fi
note "$(cat /etc/machine-id)"

# ===========================================================================
log "Hostname"
# ===========================================================================
if [ -n "$NEW_HOSTNAME" ]; then
    echo "$NEW_HOSTNAME" > /etc/hostname
elif [ ! -s /etc/hostname ] || [ "$(cat /etc/hostname 2>/dev/null)" = localhost ]; then
    echo "archlinux-ish" > /etc/hostname
fi
hostname "$(cat /etc/hostname)" 2>/dev/null || true
_hn="$(cat /etc/hostname 2>/dev/null)"
if [ -n "$_hn" ] && ! grep -qE "[[:space:]]$_hn(\$|[[:space:]])" /etc/hosts 2>/dev/null; then
    printf '127.0.1.1\t%s\n' "$_hn" >> /etc/hosts
fi
note "$(cat /etc/hostname)"

# ===========================================================================
log "sudo for the wheel group"
# ===========================================================================
grep -q '^@includedir /etc/sudoers.d\|^#includedir /etc/sudoers.d' /etc/sudoers 2>/dev/null || \
    echo '@includedir /etc/sudoers.d' >> /etc/sudoers
if [ "$SUDO_NOPASSWD" = 1 ]; then
    echo '%wheel ALL=(ALL:ALL) NOPASSWD: ALL' > /etc/sudoers.d/aok-wheel
    note "passwordless sudo for %wheel"
else
    echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/aok-wheel
    note "%wheel sudo (password required; set SUDO_NOPASSWD=1 for passwordless)"
fi
chmod 0440 /etc/sudoers.d/aok-wheel
if command -v visudo >/dev/null 2>&1 && ! visudo -cf /etc/sudoers.d/aok-wheel >/dev/null 2>&1; then
    rm -f /etc/sudoers.d/aok-wheel
    note "WARNING: generated sudoers fragment failed validation; removed it"
fi
if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx wheel || usermod -aG wheel "$TARGET_USER" 2>/dev/null
    note "$TARGET_USER is in: $(id -nG "$TARGET_USER")"
fi

# ===========================================================================
log "Login shells -> bash"
# ===========================================================================
grep -qx /bin/bash /etc/shells 2>/dev/null || echo /bin/bash >> /etc/shells
for u in root $TARGET_USER; do
    id "$u" >/dev/null 2>&1 || continue
    chsh -s /bin/bash "$u" >/dev/null 2>&1 || usermod -s /bin/bash "$u" 2>/dev/null || true
done
note "root + ${TARGET_USER:-} now use bash"

# ===========================================================================
log "MOTD"
# ===========================================================================
cat > /etc/motd <<'MOTD'

   Arch Linux  .  iSH-AOK  (terminal-only userspace, EXPERIMENTAL)
   ------------------------------------------------------------
   services :  no working init under iSH -- see start-aok-services
   status   :  pgrep -fl 'sshd|syslog-ng|chronyd|crond'
   time     :  chronyc -h 127.0.0.1 tracking    docs : man <command>
   ------------------------------------------------------------

MOTD

# ===========================================================================
log "Shell niceties (/etc/profile.d)"
# ===========================================================================
cat > /etc/profile.d/30-aok-niceties.sh <<'NICETIES'
# AOK "full Linux feel" interactive niceties.  Safe for dash & bash.
export EDITOR=vim VISUAL=vim PAGER=less
export LESS='-R -M -i'
export TERM="${TERM:-xterm-256color}"
export LC_ALL="${LC_ALL:-C.UTF-8}" LANG="${LANG:-C.UTF-8}"

case $- in *i*) ;; *) return 2>/dev/null || exit 0;; esac

alias ls='ls --color=auto'
alias ll='ls -alF --color=auto'
alias la='ls -A --color=auto'
alias l='ls -CF --color=auto'
alias grep='grep --color=auto'
alias df='df -h'
alias free='free -m'
alias ..='cd ..'
alias ...='cd ../..'

if [ -n "${BASH:-}" ]; then
  if [ "$(id -u)" = 0 ]; then
    PS1='\[\e[1;31m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]# '
  else
    PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
  fi
  HISTSIZE=5000; HISTFILESIZE=10000; HISTCONTROL=ignoreboth
  shopt -s histappend checkwinsize 2>/dev/null
  [ -f /usr/share/bash-completion/bash_completion ] && . /usr/share/bash-completion/bash_completion
fi

if [ -z "${_AOK_SUMMARY_DONE:-}" ]; then
  export _AOK_SUMMARY_DONE=1
  printf '\n  \033[1;36m%s\033[0m  .  kernel \033[1m%s\033[0m  .  %s\n' \
    "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-Arch Linux}")" \
    "$(uname -r)" "$(uname -m)"
  printf '  uptime:%s\n' "$(uptime 2>/dev/null | sed 's/^[[:space:]]*//;s/^/ /')"
  printf '  disk /: %s   mem: %s\n\n' \
    "$(command df -h / 2>/dev/null | awk 'NR==2{print $3" / "$2" ("$5")"}')" \
    "$(command free -m 2>/dev/null | awk '/^Mem:/{print $3"M / "$2"M"}')"
fi
NICETIES
chmod 0644 /etc/profile.d/30-aok-niceties.sh

cat > /etc/profile.d/40-aok-tools.sh <<'TOOLS'
# Interactive niceties for the installed CLI tool set. Safe for dash & bash.
case $- in *i*) ;; *) return 2>/dev/null || exit 0;; esac

command -v dircolors >/dev/null 2>&1 && eval "$(dircolors -b 2>/dev/null)"
alias ip='ip -color=auto'
if command -v bat >/dev/null 2>&1; then export BAT_THEME=ansi; fi

if [ -n "${BASH:-}" ]; then
  for f in /usr/share/fzf/key-bindings.bash /usr/share/fzf/completion.bash; do
    [ -f "$f" ] && . "$f"
  done
  export FZF_DEFAULT_OPTS="--height 40% --layout=reverse --border"
  command -v fd >/dev/null 2>&1 && export FZF_DEFAULT_COMMAND='fd --type f'
fi
TOOLS
chmod 0644 /etc/profile.d/40-aok-tools.sh
note "wrote /etc/profile.d/30-aok-niceties.sh and 40-aok-tools.sh"

# ===========================================================================
log "chrony config (iSH-aware: monitor only, host owns the clock)"
# ===========================================================================
# In iSH the guest system clock IS the host (iOS) clock, already NTP-synced by
# iOS. chronyd therefore runs with -x (no clock control). Under iSH the unix
# command socket does not work, so reach chronyc over the localhost UDP port:
# chronyc -h 127.0.0.1 tracking
if [ -d /etc ]; then
    cat > /etc/chrony.conf <<'CHRONYCONF'
# chrony.conf -- tuned for iSH-AOK (monitoring mode; chronyd started with -x)
pool pool.ntp.org iburst
server time.cloudflare.com iburst
server time.google.com iburst
driftfile /var/lib/chrony/chrony.drift
logdir /var/log/chrony
# NB: no 'rtcsync' / 'initstepslew' (no clock control under iSH).
CHRONYCONF
    mkdir -p /var/lib/chrony /var/log/chrony
    chown chrony:chrony /var/lib/chrony /var/log/chrony 2>/dev/null || true
    note "wrote /etc/chrony.conf (monitor only); started with -x below"
fi

# ===========================================================================
log "Periodic maintenance cron (run-parts /etc/cron.*)"
# ===========================================================================
mkdir -p /etc/cron.hourly /etc/cron.daily /etc/cron.weekly /etc/cron.monthly \
         /var/spool/cron
if [ ! -s /var/spool/cron/root ] || \
   ! grep -q 'run-parts' /var/spool/cron/root 2>/dev/null; then
    cat > /var/spool/cron/root <<'CRONTAB'
# Arch has no /etc/crontab equivalent of Debian's run-parts wiring; do it
# ourselves so logrotate etc. under /etc/cron.* actually run.
0     *  *  *  *  run-parts /etc/cron.hourly
0     2  *  *  *  run-parts /etc/cron.daily
0     3  *  *  6  run-parts /etc/cron.weekly
0     5  1  *  *  run-parts /etc/cron.monthly
CRONTAB
    chmod 0600 /var/spool/cron/root
    chown root:root /var/spool/cron/root
    note "installed root maintenance crontab"
else
    note "root crontab already has periodic entries; left as-is"
fi

# ===========================================================================
log "Neovim starter config"
# ===========================================================================
NVIM_MARKER="-- AOK starter config (provision-ultimate-archlinux.sh)"
write_nvim() {  # <homedir> <owner>
    _hd="$1"; _own="$2"
    [ -n "$_hd" ] || return 0
    _cfg="$_hd/.config/nvim/init.lua"
    # -e: the marker starts with "--", which grep would otherwise read as options.
    if [ -f "$_cfg" ] && ! grep -qF -e "$NVIM_MARKER" "$_cfg" 2>/dev/null; then
        note "nvim: keeping your existing $_cfg"
        return 0
    fi
    mkdir -p "$_hd/.config/nvim"
    cat > "$_cfg" <<'NVIMCFG'
-- AOK starter config (provision-ultimate-archlinux.sh)
-- Dependency-free Neovim starter. Edit freely; the provisioner only overwrites
-- this file while the marker line above is present (delete it to keep yours).

vim.g.mapleader = " "
vim.g.maplocalleader = " "

local o = vim.opt
o.number = true
o.relativenumber = true
o.mouse = "a"
o.ignorecase = true
o.smartcase = true
o.incsearch = true
o.hlsearch = true
o.expandtab = true
o.shiftwidth = 4
o.tabstop = 4
o.softtabstop = 4
o.smartindent = true
o.breakindent = true
o.wrap = false
o.scrolloff = 5
o.sidescrolloff = 8
o.termguicolors = true
o.signcolumn = "yes"
o.cursorline = true
o.splitright = true
o.splitbelow = true
o.undofile = true
o.swapfile = false
o.updatetime = 300
o.timeoutlen = 500
o.completeopt = "menuone,noselect"
o.list = true
o.listchars = { tab = "» ", trail = "·", nbsp = "␣" }
o.title = true

pcall(vim.cmd.colorscheme, "habamax")

-- netrw as a light built-in file explorer
vim.g.netrw_banner = 0
vim.g.netrw_liststyle = 3

-- yank to the host clipboard over the terminal (OSC52) on Neovim >= 0.10
if vim.fn.has("nvim-0.10") == 1 then
  local ok, osc52 = pcall(require, "vim.ui.clipboard.osc52")
  if ok then
    vim.g.clipboard = {
      name = "OSC52",
      copy = { ["+"] = osc52.copy("+"), ["*"] = osc52.copy("*") },
      paste = { ["+"] = osc52.paste("+"), ["*"] = osc52.paste("*") },
    }
  end
end

local map = vim.keymap.set
map("n", "<leader>w", "<cmd>write<cr>", { desc = "Save" })
map("n", "<leader>q", "<cmd>quit<cr>", { desc = "Quit" })
map("n", "<leader>Q", "<cmd>quitall!<cr>", { desc = "Quit all (force)" })
map("n", "<leader>e", "<cmd>Explore<cr>", { desc = "File explorer" })
map("n", "<esc>", "<cmd>nohlsearch<cr>", { silent = true })
map("n", "<C-h>", "<C-w>h"); map("n", "<C-j>", "<C-w>j")
map("n", "<C-k>", "<C-w>k"); map("n", "<C-l>", "<C-w>l")
map("v", "J", ":m '>+1<cr>gv=gv", { silent = true })
map("v", "K", ":m '<-2<cr>gv=gv", { silent = true })
map("x", "<leader>p", [["_dP]], { desc = "Paste without losing register" })
map({ "n", "v" }, "<leader>y", [["+y]], { desc = "Yank to host clipboard" })

local aug = vim.api.nvim_create_augroup("aok", { clear = true })
vim.api.nvim_create_autocmd("TextYankPost", {
  group = aug,
  callback = function() vim.highlight.on_yank({ timeout = 200 }) end,
})
vim.api.nvim_create_autocmd("BufReadPost", {
  group = aug,
  callback = function()
    local m = vim.api.nvim_buf_get_mark(0, '"')
    if m[1] > 0 and m[1] <= vim.api.nvim_buf_line_count(0) then
      pcall(vim.api.nvim_win_set_cursor, 0, m)
    end
  end,
})

-- Optional plugin manager (lazy.nvim) -- uncomment to enable (needs network):
-- local lazypath = vim.fn.stdpath("data") .. "/lazy/lazy.nvim"
-- if not (vim.uv or vim.loop).fs_stat(lazypath) then
--   vim.fn.system({ "git", "clone", "--filter=blob:none",
--     "https://github.com/folke/lazy.nvim.git", "--branch=stable", lazypath })
-- end
-- vim.opt.rtp:prepend(lazypath)
-- require("lazy").setup({ --[[ plugin specs here ]] })
NVIMCFG
    chown -R "$_own" "$_hd/.config" 2>/dev/null || true
    note "nvim: wrote $_cfg"
}
write_nvim /root root
[ -n "$TARGET_HOME" ] && write_nvim "$TARGET_HOME" "$TARGET_USER"

# ===========================================================================
log "tmux config"
# ===========================================================================
TMUX_MARKER="# AOK tmux.conf (provision-ultimate-archlinux.sh)"
write_tmux() {  # <homedir> <owner>
    _hd="$1"; _own="$2"
    [ -n "$_hd" ] || return 0
    _cfg="$_hd/.tmux.conf"
    if [ -f "$_cfg" ] && ! grep -qF -e "$TMUX_MARKER" "$_cfg" 2>/dev/null; then
        note "tmux: keeping your existing $_cfg"
        return 0
    fi
    cat > "$_cfg" <<'TMUXCONF'
# AOK tmux.conf (provision-ultimate-archlinux.sh)
# This file was "stolen" from
# https://www.hamvocke.com/blog/a-guide-to-customizing-your-tmux-conf/
#
# Enable mouse mode (tmux 2.1 and above)
set -g mouse on

setw -g mode-keys vi
bind-key -T copy-mode-vi MouseDragEnd1Pane send-keys -X copy-pipe-and-cancel "pbcopy"

# Map escape to caps lock
#set-option -g prefix Escape
#unbind-key C-b
#bind-key Escape send-prefix

# reload config file (change file location to your the tmux.conf you want to use)
bind r source-file ~/.tmux.conf

# split panes using | and -
bind | split-window -h
bind - split-window -v
unbind '"'
unbind %

#
# if defined use custom handling for navigation keys,
# set by /usr/local/bin/nav_keys.shell
#
run-shell "[ -f /etc/opt/AOK/tmux_nav_key_handling ] && tmux source /etc/opt/AOK/tmux_nav_key_handling"

# # switch panes using Alt-arrow without prefix
# bind -n M-Left select-pane -L
# bind -n M-Right select-pane -R
# bind -n M-Up select-pane -U
# bind -n M-Down select-pane -D

# don't rename windows automatically
set-option -g allow-rename off

######################
### DESIGN CHANGES ###
######################

# loud or quiet?
set -g visual-activity off
set -g visual-bell off
set -g visual-silence off
setw -g monitor-activity off
set -g bell-action none

#  modes
setw -g clock-mode-colour colour5
setw -g mode-style 'fg=colour1 bg=colour18 bold'

# panes
set -g pane-border-style 'fg=colour19 bg=colour0'
set -g pane-active-border-style 'bg=colour0 fg=colour9'

# statusbar
set -g status-position bottom
set -g status-justify left
set -g status-style 'bg=colour18 fg=colour137 dim'
set -g status-left ''
set -g status-right '#[fg=colour233,bg=colour19] %d/%m #[fg=colour233,bg=colour8] %H:%M:%S '
set -g status-right-length 50
set -g status-left-length 20

setw -g window-status-current-style 'fg=colour1 bg=colour19 bold'
setw -g window-status-current-format ' #I#[fg=colour249]:#[fg=colour255]#W#[fg=colour249]#F '

setw -g window-status-style 'fg=colour9 bg=colour18'
setw -g window-status-format ' #I#[fg=colour237]:#[fg=colour250]#W#[fg=colour244]#F '

setw -g window-status-bell-style 'fg=colour255 bg=colour1 bold'

# messages
set -g message-style 'fg=colour232 bg=colour16 bold'
TMUXCONF
    chown "$_own" "$_cfg" 2>/dev/null || true
    note "tmux: wrote $_cfg"
}
write_tmux /root root
[ -n "$TARGET_HOME" ] && write_tmux "$TARGET_HOME" "$TARGET_USER"

# ===========================================================================
log "Services"
# ===========================================================================
# Unlike Alpine (OpenRC) and Devuan (sysvinit), stock Arch ships NO working
# non-systemd init or service supervisor -- systemd is Arch's only supported
# init, and iSH-AOK does not run the guest's systemd as PID 1 (iSH provides
# its own emulated boot process instead). `systemctl enable/start` therefore
# cannot work here: there is no systemd manager socket for it to talk to.
#
# Rather than pretend otherwise, this writes a small idempotent launcher
# (/usr/local/sbin/start-aok-services) that starts the same daemons Alpine/
# Devuan's provisioners enable at boot directly in the background, and calls
# it once now. There is no persistent per-boot trigger (iSH has nothing
# equivalent to init to hook), so re-run it yourself after each fresh app
# launch -- e.g. `sudo start-aok-services`, or add that line to your shell
# profile if you want it automatic on your first login each session.
cat > /usr/local/sbin/start-aok-services <<'STARTSVC'
#!/bin/sh
# Idempotent: only starts a daemon if it isn't already running.
running() { pgrep -x "$1" >/dev/null 2>&1; }
mkdir -p /run/sshd
if ! running sshd; then
    /usr/bin/sshd
    echo "started sshd"
fi
if ! running syslog-ng && [ -x /usr/bin/syslog-ng ]; then
    /usr/bin/syslog-ng
    echo "started syslog-ng"
fi
if ! running chronyd && [ -x /usr/bin/chronyd ]; then
    /usr/bin/chronyd -x
    echo "started chronyd -x"
fi
if ! running crond && [ -x /usr/sbin/crond ]; then
    /usr/sbin/crond
    echo "started crond"
fi
STARTSVC
chmod 0755 /usr/local/sbin/start-aok-services
note "wrote /usr/local/sbin/start-aok-services"
/usr/local/sbin/start-aok-services | sed 's/^/    /'

# ===========================================================================
log "Done"
# ===========================================================================
printf '    %s\n' "$(date)"
note "Running services:"
pgrep -fl 'sshd|syslog-ng|chronyd|crond' 2>/dev/null | awk '{print $2}' | sort -u | tr '\n' ' ' | sed 's/^/      /'; echo
cat <<EOF

    Next:
      * Re-login to pick up bash + the new prompt/MOTD.
      * 'chronyc -h 127.0.0.1 tracking' / '... sources' to see NTP status.
      * No init under iSH: after each fresh app launch, run
        'sudo start-aok-services' again to bring daemons back up.
EOF
