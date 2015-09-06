
#
# Test UDP submit functions
#

use Test;
BEGIN { plan tests => 5 + 2 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS;
use IO::Socket::INET;
use Time::HiRes qw(sleep);

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "OH0CAL-3";
my $server_call = "TESTING";

# full feed client
my $i_rx = new Ham::APRS::IS("localhost:55152", "OH0CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# udp setup
$usock = IO::Socket::INET->new(Proto => 'udp', PeerPort => 55080, PeerAddr => "127.0.0.1");
ok(defined $usock, (1), "Failed to set up an UDP client socket");

# test ###########################

my $data = "TEST>UDAPRS,TCPIP*:>udp packet content";
my $out = "TEST>UDAPRS,TCPIP*,qAU,TESTING:>udp packet content";
my $post = "user TEST pass 29939 vers udpaprstester 1.0\r\n"
	. "$data";

$usock->send($post);

my $l = $i_rx->getline_noncomment();
ok($l, $out, "Got wrong line when sent using UDP");

# test rejections

my $fail = 0;
my(@udp_rej) = (
	[ 'UDROP', '', "UDROP>UDAPRS:>udp packet rejected, no passcode" ],
	[ 'UDROP', 123, "UDROP>UDAPRS:>udp packet rejected, bad passcode" ],
	[ 'UDROPASDSJSKD', 13293, "UDROPASDSJSKD>UDAPRS:>udp packet rejected, too long login callsign" ],
	[ 'UDRO/OH', 29929, "UDROP>UDAPRS:>udp packet rejected, invalid login callsign" ],
);

for my $rej (@udp_rej) {
	my($src, $passcode, $rejdata) = @{ $rej };
	my $rej_post = "user $src pass $passcode vers udpaprstester 1.0\r\n"
		. "$rejdata";

	$usock->send($rej_post);
}

# another success after the failures

$data = "TEST>UDAPRS:>udp packet content 2";
$out = "TEST>UDAPRS,TCPIP*,qAU,TESTING:>udp packet content 2";
$post = "user TEST pass 29939 vers udpaprstester 1.0\r\n"
	. "$data\r\n";

$usock->send($post);

$l = $i_rx->getline_noncomment();
ok($l, $out, "Got wrong line when sent using UDP");


# disconnect ####################

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

