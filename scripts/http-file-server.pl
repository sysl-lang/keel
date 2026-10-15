#!/usr/bin/perl
# A web server on the host for keel's wget test (milestone 11b): on 127.0.0.1:<port>, which keel reaches
# as 10.0.2.2:<port> through slirp, every GET of /<name> is answered with the file <file> (HTTP/1.0, its
# length given), anything else with 404. One connection at a time; it runs until killed. (Python's
# http.server would do, but it asks the resolver for the address's name first, which can hang.)
#
#   perl http-file-server.pl <port> <name> <file>

use strict;
use warnings;
use IO::Socket::INET;

$SIG{PIPE} = 'IGNORE';
my ($port, $name, $file) = @ARGV;
open(my $f, '<:raw', $file) or die "http-file-server: $file: $!\n";
my $body = do { local $/; <$f> };
close $f;

my $server = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => $port, Proto => 'tcp',
    Listen => 4, ReuseAddr => 1) or die "http-file-server: port $port: $!\n";
$| = 1;
print "http-file-server: serving /$name on 127.0.0.1:$port\n";
while (my $c = $server->accept) {
    my $request = '';
    while ($request !~ /\r?\n\r?\n/) {
        my $n = sysread($c, $request, 4096, length $request);
        last if !$n;
    }
    my ($path) = $request =~ m{^GET\s+(\S+)};
    if (defined $path && $path eq "/$name") {
        print $c "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " . length($body)
            . "\r\nConnection: close\r\n\r\n";
        print $c $body;
        print "http-file-server: sent /$name, ", length($body), " bytes\n";
    } else {
        print $c "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        print "http-file-server: 404 for ", ($path // '?'), "\n";
    }
    close $c;
}
