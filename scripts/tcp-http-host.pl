#!/usr/bin/perl
# The host's half of keel's HTTP test (milestone 19). keel's session starts a background `busybox nc -l`
# on a port QEMU forwards from 127.0.0.1:<http port>, its input an HTTP/1.0 response whose body is
# /bin/busybox, then waits in a second `busybox nc -l` on another forwarded port. This fetches the
# response with curl — retrying while keel is not listening yet — compares the body byte for byte with
# <expected>, then tells keel the outcome through nc: it connects, waits for the "go" keel's nc sends
# first (so it knows the line reaches a listening nc rather than a refused connection), sends one line
# and half-closes. keel's transcript then holds that line.
#
#   perl tcp-http-host.pl <http port> <nc port> <expected file> <scratch file> <seconds>

use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;
use Time::HiRes qw(time sleep);

$SIG{PIPE} = 'IGNORE';
my ($http_port, $nc_port, $expected, $scratch, $limit) = @ARGV;
my $deadline = time + $limit;

sub slurp {
    my ($name) = @_;
    open(my $f, '<:raw', $name) or return undef;
    local $/;
    my $all = <$f>;
    close $f;
    return $all;
}

my $want = slurp($expected);
my $verdict;
while (time < $deadline) {
    unlink $scratch;
    my $rc = system('curl', '-s', '-f', '-m', '20', '-o', $scratch, "http://127.0.0.1:$http_port/busybox");
    if ($rc == 0) {
        my $got = slurp($scratch);
        if (defined $got && $got eq $want) {
            $verdict = "host: curl fetched " . length($got) . " bytes from busybox nc on keel, the same as /bin/busybox";
        } else {
            $verdict = "host: curl FAILED: what keel's busybox nc sent (" . length($got // '') . " bytes) is not /bin/busybox";
        }
        last;
    }
    sleep 0.3;
}
if (!defined $verdict) {
    print "host: curl FAILED: keel's busybox nc never answered on port $http_port\n";
    exit 1;
}
print "$verdict\n";

while (time < $deadline) {
    my $s = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => $nc_port, Proto => 'tcp', Timeout => 2);
    if (!$s) {
        sleep 0.2;
        next;
    }
    my $sel = IO::Select->new($s);
    my $go = '';
    while ($go !~ /\n/ && $sel->can_read(3)) {
        my $n = sysread($s, $go, 16, length $go);
        last if !$n;
    }
    if ($go ne "go\n") {
        close $s;
        sleep 0.2;
        next;
    }
    syswrite($s, "$verdict\n");
    shutdown($s, 1);
    my $rest;
    while ($sel->can_read(3)) {
        last if !sysread($s, $rest, 64);
    }
    close $s;
    print "host: told keel's nc\n";
    exit($verdict =~ /FAILED/ ? 1 : 0);
}
print "host: curl's verdict never reached keel's nc on port $nc_port\n";
exit 1;
