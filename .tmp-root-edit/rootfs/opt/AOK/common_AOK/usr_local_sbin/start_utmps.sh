#!/bin/sh
#
# Start utmps helper sockets early enough that util-linux getty/login can
# update utmp/wtmp without printing UNIX socket errors during boot.

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

ipcserver="$(command -v s6-ipcserver 2>/dev/null)"
utmpd="$(command -v utmps-utmpd 2>/dev/null)"
wtmpd="$(command -v utmps-wtmpd 2>/dev/null)"

[ -n "$ipcserver" ] || exit 0
[ -n "$utmpd" ] || exit 0
[ -n "$wtmpd" ] || exit 0

mkdir -p /run/utmps
cd /run/utmps || exit 0

# Clear stale sockets left behind by unclean shutdowns before starting the
# superservers. If they are already running, bind will simply fail and the
# existing service keeps serving clients.
rm -f .utmpd-socket .wtmpd-socket

"$ipcserver" .utmpd-socket "$utmpd" >/dev/null 2>&1 &
"$ipcserver" .wtmpd-socket "$wtmpd" wtmp >/dev/null 2>&1 &

exit 0
