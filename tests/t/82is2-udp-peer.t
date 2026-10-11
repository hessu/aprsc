
#
# Test IS2 UDP core peers. While at it, check that the
# basic loop prevention rules work.
#
# 1) Traffic from upstreams goes to clients.
# 2) Traffic from core peers goes to clients.
# 3) Traffic does not pass between peers and upstreams.
# 4) Traffic from clients goes to core peers and upstreams.
#
# The testing order is selected so that the last packet proves
# the previous ones were not transitted to the wrong sockets.
#

use is2skip;
use Test;
BEGIN { plan tests => 13 + 2 + 9 + 8 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS2;
use Ham::APRS::IS;
use Ham::APRS::IS_Fake;
use Time::HiRes qw(sleep);

my $p = new runproduct('is2-basic');

# UDP peer socket
my $is2_hmac_keys = {
    1 => "firstkey",
    2 => "secondkey",
    3 => "thirdkey",
};
my $udp = new Ham::APRS::IS2('127.0.0.1:16405', 'TESTIN-2', 'udp-peer' => 1, 'hmac-keys' => $is2_hmac_keys, 'hmac-key-tx' => 2);
ok(defined $udp, 1, "Failed to initialize Ham::APRS::IS2 UDP server socket");
ok($udp->bind_and_listen(), 1, "Failed to bind UDP server socket");
$udp->set_destination('127.0.0.1:16404', 'TESTING');

# second UDP peer in the same group, sharing the server's socket
my $udp2 = new Ham::APRS::IS2('127.0.0.1:16406', 'TESTIN-3', 'udp-peer' => 1, 'hmac-keys' => $is2_hmac_keys, 'hmac-key-tx' => 3);
ok(defined $udp2, 1, "Failed to initialize second Ham::APRS::IS2 UDP server socket");
ok($udp2->bind_and_listen(), 1, "Failed to bind second UDP server socket");
$udp2->set_destination('127.0.0.1:16404', 'TESTING');

# TCP server socket
my $upstream_call = 'FAKEUP';
my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:10153', $upstream_call);
ok(defined $iss1, 1, "Test failed to initialize listening server socket (IPv4)");
$iss1->bind_and_listen();

# Start software
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# Set up client and connect
my $login = "N5CAL-1";
my $server_call = "TESTING";
my $client = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $client, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $client->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $client->{'error'});

# Accept connection from server
my $is1 = $iss1->accept();
ok(defined $is1, (1), "Failed to accept connection 1 from server");
ok($iss1->process_login($is1), 'ok', "Failed to accept login 1 from server");

ok($udp->is2_corepeer_negotiate(), 1, "Failed to negotiate IS2 corepeer link: " . $udp->{'error'});
ok($udp2->is2_corepeer_negotiate(), 1, "Failed to negotiate IS2 corepeer link with the second peer: " . $udp2->{'error'});

# Oversized datagrams, which do not fit in the server's input buffer, must be
# dropped: with MSG_TRUNC, Linux reports the full length of a truncated datagram.
# The traffic tests below verify that the server survived them.
my $big = "# PEER2 v 2 hello TESTIN-2 " . ('x' x 9000);
ok($udp->send_udp_packet($big), length($big), "Failed to send oversized control datagram");
$big = chr(0x02) . chr(0x00) . chr(0x23) . chr(0x10) . ('y' x 9000);
ok($udp->send_udp_packet($big), length($big), "Failed to send oversized IS2 datagram");

# test ###########################

my $s;

# 1) from upstream to client
$s = "SRC>DST,qAR,IGATE:upstream to client";
istest::txrx(\&ok, $is1, $client, $s, $s);

# 2) from core peer to client, with trace
$s = "SRC>DST,qAI,IGATE,SRV1:core peer to client, 1";
my $rx = "SRC>DST,qAI,IGATE,SRV1,TESTING:core peer to client, 1";
istest::txrx(\&ok, $udp, $client, $s, $rx);

# 3) from core peer to client
$s = "SRC>DST,qAR,IGATE:core peer to client, 2";
istest::txrx(\&ok, $udp, $client, $s, $s);

# 3b) from the second core peer to client
$s = "SRC>DST,qAR,IGATE:core peer 2 to client";
istest::txrx(\&ok, $udp2, $client, $s, $s);

# 4) from client to peers and upstreams
$s = "SRC>DST,qAR,IGATE:from client";
istest::txrx(\&ok, $client, $is1, $s, $s);

my @r = $udp->get_packets();
ok($#r, 0, "Failed to pass packet from client to UDP peer, packet list len < 0");
ok($r[0], $s, "Failed to pass packet from client to UDP peer");

@r = $udp2->get_packets();
ok($#r, 0, "Failed to pass packet from client to second UDP peer, packet list len < 0");
ok($r[0], $s, "Failed to pass packet from client to second UDP peer");

# 5) frame authentication: the signature covers a timestamp, and a frame
# whose timestamp is more than 10 seconds from the server's clock is dropped,
# so that captured frames can not be replayed later. Frames within the
# window pass.

sub signed_frame($$$)
{
	my($peer, $packet, $time) = @_;

	my $im = APRSIS2::IS2Message->new({
		'type' => APRSIS2::IS2Message::Type::IS_PACKET(),
		'is_packet' => [ APRSIS2::ISPacket->new({
			'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
			'is_packet_data' => $packet,
		}) ],
	});

	return $peer->is2_udp_sign($peer->is2_frame_wrap($im->encode), $time);
}

# Both sides work in whole seconds, so stay a second away from the limit.
foreach my $offset (-12, 12, -8, 8) {
	$s = "SRC>DST,qAR,IGATE:timestamp offset $offset";
	ok($udp->send_udp_packet(signed_frame($udp, $s, time() + $offset)) > 0, 1, "Failed to send frame with timestamp offset $offset");
	if ($offset == -12 || $offset == 12) {
		# must be dropped: the helper packet must be the next one received
		my $helper = "SRC>DST,qAR,IGATE:helper after offset $offset";
		istest::txrx(\&ok, $udp, $client, $helper, $helper);
	} else {
		istest::rx(\&ok, $client, $s, $s);
	}
}

# disconnect ####################

$ret = $client->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $client->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

