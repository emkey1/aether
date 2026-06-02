#!/usr/bin/perl
use strict;
use warnings;

use Fcntl qw(F_SETFD FD_CLOEXEC);
use POSIX qw(:sys_wait_h);

$| = 1;

pipe(my $status_rh, my $status_wh) or die "pipe: $!";
fcntl($status_rh, F_SETFD, FD_CLOEXEC) or die "fcntl status_rh: $!";
fcntl($status_wh, F_SETFD, FD_CLOEXEC) or die "fcntl status_wh: $!";

my %child_status;
$SIG{'CHLD'} = sub {
    while ((my $pid = waitpid(-1, WNOHANG)) > 0) {
        $child_status{$pid} = $?;
    }
};

my $wrapper_pid = fork();
defined $wrapper_pid or die "fork wrapper: $!";

if ($wrapper_pid == 0) {
    close $status_rh;

    my $server_pid = fork();
    defined $server_pid or die "fork server: $!";
    if ($server_pid == 0) {
        exec "/bin/sleep", "30";
        die "exec sleep server: $!";
    }

    my $session_pid = fork();
    defined $session_pid or die "fork session: $!";
    if ($session_pid == 0) {
        exec "/bin/sleep", "30";
        die "exec sleep session: $!";
    }

    my $alarm = 0;
    $SIG{'ALRM'} = sub { $alarm = 1; };
    alarm 3;
    while (!$alarm && !defined $child_status{$session_pid}) {
        sleep 3600;
    }
    alarm 0;
    syswrite $status_wh, "OK!";

    while (!defined $child_status{$server_pid} &&
           !defined $child_status{$session_pid}) {
        sleep 3600;
    }
    exit 0;
}

close $status_wh;
my $status = "";
my $n = sysread($status_rh, $status, 3);
die "sysread failed: $!" unless defined $n;
print "status_bytes=$n status=[$status]\n";
waitpid($wrapper_pid, 0);
