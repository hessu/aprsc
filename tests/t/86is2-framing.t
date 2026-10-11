
#
# Test IS2 framing errors: the server must disconnect the client,
# must not process any further frames from it, and must survive.
#

use strict;
use Test;
BEGIN { plan tests => 4 + 3 + 2 + 1 };
use runproduct;
use istest;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "OH7LZB";
my $is = new Ham::APRS::IS2("localhost:56152", $login);
ok(defined $is, 1, "Failed to initialize Ham::APRS::IS2");

my $ret = $is->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $is->{'error'});

# a valid message body, framed with a non-zero reserved byte, followed by
# the same body framed correctly

my $im = APRSIS2::IS2Message->new({
	'type' => APRSIS2::IS2Message::Type::IS_PACKET(),
	'is_packet' => [ APRSIS2::ISPacket->new({
		'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
		'is_packet_data' => "$login>APRS,TCPIP*:>framing test",
	}) ],
});
my $body = $im->encode;
my $bad  = chr(0x02) . chr(0x01) . pack('n', length($body)) . $body . chr(0x03);
my $good = chr(0x02) . chr(0x00) . pack('n', length($body)) . $body . chr(0x03);

# The server closes the connection on the first frame, and must not parse
# the second one, since the client object is gone.
ok($is->send_raw($bad . $good), 1, "Failed to send frames with a non-zero reserved byte");

my $r = $is->get_packet(3);
ok($r, undef, "Received a packet after a framing error");
ok($is->{'state'}, 'disconnected', "Server did not disconnect the client after a framing error: " . $is->{'error'});

# the server must still be alive and accepting clients

$ret = $is->connect('retryuntil' => 8);
ok($ret, 1, "Failed to reconnect to the server: " . $is->{'error'});

$ret = $is->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $is->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

