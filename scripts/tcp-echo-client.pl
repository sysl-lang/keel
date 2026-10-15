#!/usr/bin/perl
# The host's half of keel's TCP echo test (milestone 19): connect to 127.0.0.1:<port>, which QEMU's
# user-mode network forwards to keel, wait for keel's `linuxtcp echo` to say "keel ready", then send
# <bytes> bytes of a deterministic stream while reading the echo back, half-close, and check every byte
# that comes back, in order, until keel's half-close. Writing and reading share one select loop, so
# neither side's windows can deadlock the other.
#
#   perl tcp-echo-client.pl <port> <bytes> <seconds> <label>
#
# keel may not be listening yet: slirp accepts the host's connection at once and only then tries keel,
# whose RST closes it again, so a connection that ends before the banner is simply tried again until
# <seconds> have passed. Prints "host: tcp echo ok (<label>) <n> bytes" or a FAILED line.

use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;
use Time::HiRes qw(time sleep);

$SIG{PIPE} = 'IGNORE';
my ($port, $bytes, $limit, $label) = @ARGV;
my $deadline = time + $limit;

# The stream: 64 KiB blocks of one random block, each rotated by its number and starting with that
# number, so a block out of place or a byte off by one shows.
srand(19);
my $base = join '', map { chr(int(rand(256))) } 1 .. 65536;
my $twice = $base . $base;
my $data = '';
for (my $b = 0; length($data) < $bytes; $b++) {
    my $block = substr($twice, $b % 65521, 65536);
    substr($block, 0, 4) = pack('N', $b);
    $data .= $block;
}
$data = substr($data, 0, $bytes);

while (time < $deadline) {
    my $s = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => $port, Proto => 'tcp', Timeout => 2);
    if (!$s) {
        sleep 0.2;
        next;
    }
    my $sel = IO::Select->new($s);
    my $banner = '';
    while ($banner !~ /\n/ && $sel->can_read(3)) {
        my $n = sysread($s, $banner, 64, length $banner);
        last if !$n;
    }
    if ($banner ne "keel ready\n") {
        close $s;
        sleep 0.2;
        next;
    }
    $s->blocking(0);
    my $started = time;
    my ($sent, $got, $shut) = (0, 0, 0);
    my $ended = 0;
    while (time < $deadline) {
        my $wsel = $sent < $bytes ? IO::Select->new($s) : undef;
        my ($r, $w) = IO::Select->select($sel, $wsel, undef, 1);
        if ($w && @$w && $sent < $bytes) {
            my $n = syswrite($s, $data, 65536, $sent);
            $sent += $n if defined $n && $n > 0;
        }
        if ($sent == $bytes && !$shut) {
            shutdown($s, 1);
            $shut = 1;
        }
        if ($r && @$r) {
            my $buf = '';
            my $n = sysread($s, $buf, 65536);
            if (defined $n && $n == 0) {
                $ended = 1;
                last;
            }
            if (defined $n) {
                if (substr($data, $got, $n) ne $buf) {
                    my $i = 0;
                    $i++ while $i < $n && substr($data, $got + $i, 1) eq substr($buf, $i, 1);
                    print "host: tcp echo FAILED ($label): byte ", $got + $i, " came back wrong\n";
                    exit 1;
                }
                $got += $n;
            }
        }
    }
    close $s;
    if ($ended && $got == $bytes) {
        printf "host: tcp echo ok (%s) %d bytes in %.1f s\n", $label, $got, time - $started;
        exit 0;
    }
    print "host: tcp echo FAILED ($label): $got of $bytes bytes came back", ($ended ? '' : ' before the time ran out'), "\n";
    exit 1;
}
print "host: tcp echo FAILED ($label): keel never answered on port $port\n";
exit 1;
