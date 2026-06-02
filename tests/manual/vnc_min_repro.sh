#!/bin/sh
set -eu

display_num="${1:-1}"
geometry="${GEOMETRY:-1280x800}"
depth="${DEPTH:-24}"
localhost_mode="${LOCALHOST:-yes}"
runtime="${RUNTIME:-120}"

host="$(hostname 2>/dev/null || echo localhost)"
vnc_dir="${HOME}/.vnc"
xauth_file="${XAUTHORITY:-$HOME/.Xauthority}"
log_prefix="${vnc_dir}/min-repro-${host}-${display_num}"
server_log="${log_prefix}.server.log"
client_log="${log_prefix}.client.log"
summary_log="${log_prefix}.summary.log"
display_socket="/tmp/.X11-unix/X${display_num}"
socket_log="${log_prefix}.socket.log"

mkdir -p "$vnc_dir"
: >"$client_log"
: >"$summary_log"
: >"$socket_log"

cleanup() {
    rc=$?
    if [ -n "${xterm_pid:-}" ]; then
        kill "$xterm_pid" 2>/dev/null || true
    fi
    if [ -n "${server_pid:-}" ]; then
        kill "$server_pid" 2>/dev/null || true
        sleep 1
        kill -9 "$server_pid" 2>/dev/null || true
    fi
    rm -f "$display_socket" "/tmp/.X${display_num}-lock"
    exit "$rc"
}
trap cleanup INT TERM EXIT

find_xtigervnc() {
    for cmd in Xtigervnc Xvnc; do
        if command -v "$cmd" >/dev/null 2>&1; then
            command -v "$cmd"
            return 0
        fi
    done
    return 1
}

make_cookie() {
    if command -v mcookie >/dev/null 2>&1; then
        mcookie
        return
    fi
    od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n'
}

xtigervnc="$(find_xtigervnc || true)"
if [ -z "$xtigervnc" ]; then
    echo "No Xtigervnc/Xvnc found in PATH" >&2
    exit 1
fi

if ! command -v xauth >/dev/null 2>&1; then
    echo "xauth is required for this reproducer" >&2
    exit 1
fi

cookie="$(make_cookie)"
rm -f "$xauth_file"
xauth -f "$xauth_file" add "${host}:${display_num}" . "$cookie"
xauth -f "$xauth_file" add "${host}/unix:${display_num}" . "$cookie"

rm -f "$display_socket" "/tmp/.X${display_num}-lock" "$server_log"

echo "Starting $xtigervnc on :$display_num" | tee -a "$summary_log"
"$xtigervnc" ":${display_num}" \
    -geometry "$geometry" \
    -depth "$depth" \
    -localhost "$localhost_mode" \
    -auth "$xauth_file" \
    -SecurityTypes=None \
    >"$server_log" 2>&1 &
server_pid=$!
echo "Xtigervnc pid=$server_pid" | tee -a "$summary_log"

i=0
while [ ! -e "$display_socket" ] && [ "$i" -lt 100 ]; do
    i=$((i + 1))
    sleep 0.1
done

if [ ! -e "$display_socket" ]; then
    echo "Display socket $display_socket never appeared" | tee -a "$summary_log"
    exit 1
fi

export DISPLAY=":${display_num}"
export XAUTHORITY="$xauth_file"

echo "DISPLAY=$DISPLAY" | tee -a "$summary_log"
echo "XAUTHORITY=$XAUTHORITY" | tee -a "$summary_log"
echo "display_socket=$display_socket" | tee -a "$summary_log"
ls -l "$display_socket" "$xauth_file" >>"$summary_log" 2>&1 || true
xauth -f "$xauth_file" list >>"$summary_log" 2>&1 || true

if command -v perl >/dev/null 2>&1; then
    perl -MSocket -e '
        use strict;
        use warnings;
        my ($path) = @ARGV;
        socket(my $s, PF_UNIX, SOCK_STREAM, 0) or die "socket: $!";
        my $ok = connect($s, sockaddr_un($path));
        if ($ok) {
            print "pathname connect ok\n";
            exit 0;
        }
        print "pathname connect failed: $!\n";
        exit 1;
    ' "$display_socket" >>"$socket_log" 2>&1 || true
fi

run_client_check() {
    label="$1"
    shift
    echo "== $label ==" >>"$client_log"
    if command -v strace >/dev/null 2>&1; then
        strace -f -o "${log_prefix}.${label}.strace" "$@" >>"$client_log" 2>&1
    else
        "$@" >>"$client_log" 2>&1
    fi
    if [ "$?" -eq 0 ]; then
        echo "$label: ok" | tee -a "$summary_log"
    else
        rc=$?
        echo "$label: failed rc=$rc" | tee -a "$summary_log"
    fi
}

if command -v xdpyinfo >/dev/null 2>&1; then
    run_client_check xdpyinfo xdpyinfo
fi

if command -v xsetroot >/dev/null 2>&1; then
    run_client_check xsetroot xsetroot -solid '#204a87'
fi

if command -v xterm >/dev/null 2>&1; then
    echo "== xterm ==" >>"$client_log"
    if command -v strace >/dev/null 2>&1; then
        strace -f -o "${log_prefix}.xterm.strace" \
            xterm -geometry 80x24+10+10 -title "ish-vnc-repro" >>"$client_log" 2>&1 &
    else
        xterm -geometry 80x24+10+10 -title "ish-vnc-repro" >>"$client_log" 2>&1 &
    fi
    xterm_pid=$!
    sleep 2
    if kill -0 "$xterm_pid" 2>/dev/null; then
        echo "xterm: alive pid=$xterm_pid" | tee -a "$summary_log"
    else
        wait "$xterm_pid" || true
        echo "xterm: exited early" | tee -a "$summary_log"
    fi
fi

echo "Logs:" | tee -a "$summary_log"
echo "  server:  $server_log" | tee -a "$summary_log"
echo "  client:  $client_log" | tee -a "$summary_log"
echo "  socket:  $socket_log" | tee -a "$summary_log"
echo "  summary: $summary_log" | tee -a "$summary_log"
echo "Reproducer will keep Xtigervnc alive for ${runtime}s unless interrupted." | tee -a "$summary_log"

sleep "$runtime"
