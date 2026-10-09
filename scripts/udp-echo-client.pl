#!/usr/bin/perl
# The host's half of keel's network test: udp-echo-client.pl <port> <seconds> <name>
#
# Sends datagrams to 127.0.0.1:<port> (QEMU forwards it to a UDP port of keel's) until one comes back
# byte for byte, then three more, each of which must also come back, then "bye" until that comes back
# too, which tells keel's side to stop. Gives up after <seconds>. Prints "host: udp echo ok (<name>)" and
# exits 0 on success; says what failed and exits 1 otherwise. A datagram sent before keel is listening
# is simply lost, which is why each is repeated until it is answered.
use strict;
use warnings;
use IO::Socket::INET;
use IO::Select;
use Time::HiRes qw(time);

my ($port, $seconds, $name) = @ARGV;
my $end = time + $seconds;
my $sock = IO::Socket::INET->new(PeerAddr => "127.0.0.1", PeerPort => $port, Proto => "udp")
    or die "host: udp-echo-client: no socket: $!\n";
my $sel = IO::Select->new($sock);

# Send $msg until the same bytes come back; answers whether they did before the deadline.
sub echoed {
    my ($msg) = @_;
    while (time < $end) {
        $sock->send($msg);
        my $wait_until = time + 0.25;
        while (time < $wait_until) {
            last unless $sel->can_read($wait_until - time);
            my $reply = "";
            $sock->recv($reply, 2048);
            return 1 if $reply eq $msg;
        }
    }
    return 0;
}

my $ok = echoed("keel net test, $name, first");
for my $i (1 .. 3) {
    $ok &&= echoed("keel net test, $name, datagram $i " . ("x" x (100 * $i)));
}
$ok &&= echoed("bye");
if ($ok) {
    print "host: udp echo ok ($name)\n";
    exit 0;
}
print "host: udp echo FAILED ($name): no echo within ${seconds}s\n";
exit 1;
