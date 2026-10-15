#!/usr/bin/perl
# The host's half of keel's TCP echo test (milestone 11b): connect to 127.0.0.1:<port>, which QEMU's
# user-mode network forwards to keel, wait for keel's `linuxtcp echo` to say "keel ready", then send
# <bytes> bytes of a deterministic stream while reading the echo back, half-close, and check every byte
# that comes back, in order, until keel's half-close. Writing and reading share one select loop, so
# neither side's windows can deadlock the other.
#
#   perl tcp-echo-client.pl <port> <bytes> <seconds> <label> [<times>]
#
# keel may not be listening yet: slirp accepts the host's connection at once and only then tries keel,
# whose RST closes it again, so a connection that ends before the banner is simply tried again until
# <seconds> have passed. Prints "host: tcp echo ok (<label>) <n> bytes" or a FAILED line. With <times>
# (the boot test's KEEL_TCP_ONLY), the whole exchange is made that many times, one connection after
# another, all within <seconds>; a failure ends the run. A short echo says how far it got and how the
# connection ended, so a stall (the time ran out) and a loss (the end came early) tell themselves apart.

use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;
use Time::HiRes qw(time sleep);

$SIG{PIPE} = 'IGNORE';
my ($port, $bytes, $limit, $label, $times) = @ARGV;
$times //= 1;
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

for my $round (1 .. $times) {
    my $which = $times > 1 ? " round $round of $times" : '';
    exchange($which) or exit 1;
}
exit 0;

sub exchange {
my ($which) = @_;
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
    my ($last_byte, $error) = ($started, '');
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
            if (!defined $n && !$!{EAGAIN} && !$!{EWOULDBLOCK}) {
                # A reset, most likely: the connection is over without an end of stream.
                $error = "$!";
                last;
            }
            if (defined $n) {
                if (substr($data, $got, $n) ne $buf) {
                    my $i = 0;
                    $i++ while $i < $n && substr($data, $got + $i, 1) eq substr($buf, $i, 1);
                    print "host: tcp echo FAILED ($label$which): byte ", $got + $i, " came back wrong\n";
                    return 0;
                }
                $got += $n;
                $last_byte = time;
            }
        }
    }
    close $s;
    if ($ended && $got == $bytes) {
        printf "host: tcp echo ok (%s%s) %d bytes in %.1f s\n", $label, $which, $got, time - $started;
        return 1;
    }
    my $how = $ended ? 'then the end of the stream' : $error ne '' ? "then the error '$error'" : 'then the time ran out';
    printf "host: tcp echo FAILED (%s%s): %d of %d bytes came back (%d sent), %s; the last byte came %.1f s in, the exchange ended %.1f s in\n",
        $label, $which, $got, $bytes, $sent, $how, $last_byte - $started, time - $started;
    return 0;
}
print "host: tcp echo FAILED ($label$which): keel never answered on port $port\n";
return 0;
}
