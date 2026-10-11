
#
# Test IS2 framing errors: the server must disconnect the client right
# away, must not process any further frames from it, and must survive.
#

use strict;
use Test;
BEGIN { plan tests => 3 + 4*4 + 2 + 1 };
use runproduct;
use istest;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "OH7LZB";
my $is = new Ham::APRS::IS2("localhost:56152", $login);
ok(defined $is, 1, "Failed to initialize Ham::APRS::IS2");

# a valid message body, and the same body in a correct frame

my $im = APRSIS2::IS2Message->new({
	'type' => APRSIS2::IS2Message::Type::IS_PACKET(),
	'is_packet' => [ APRSIS2::ISPacket->new({
		'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
		'is_packet_data' => "$login>APRS,TCPIP*:>framing test",
	}) ],
});
my $body = $im->encode;
my $good = $is->is2_frame_wrap($body);

# Broken frames, each followed by the correct one. The server must close
# the connection on the broken frame, right away instead of waiting for
# more data, and must not parse the following frame.
my @cases = (
	[ 'non-zero reserved byte', chr(0x02) . chr(0x01) . pack('n', length($body)) . $body . chr(0x03) ],
	[ 'too short content length', chr(0x02) . chr(0x00) . pack('n', 2) . substr($body, 0, 2) . chr(0x03) ],
	[ 'too long content length', chr(0x02) . chr(0x00) . pack('n', 5000) . $body . chr(0x03) ],
	[ 'missing ETX', chr(0x02) . chr(0x00) . pack('n', length($body)) . $body . chr(0x04) ],
);

foreach my $case (@cases) {
	my($name, $bad) = @{ $case };

	my $ret = $is->connect('retryuntil' => 8);
	ok($ret, 1, "Failed to connect to the server before sending a frame with $name: " . $is->{'error'});

	ok($is->send_raw($bad . $good), 1, "Failed to send frames with $name");

	my $r = $is->get_packet(3);
	ok($r, undef, "Received a packet after a frame with $name");
	ok($is->{'state'}, 'disconnected', "Server did not disconnect the client after a frame with $name: " . $is->{'error'});

	$is->disconnect() if ($is->{'state'} eq 'connected');
}

# the server must still be alive and accepting clients

my $ret = $is->connect('retryuntil' => 8);
ok($ret, 1, "Failed to reconnect to the server: " . $is->{'error'});

$ret = $is->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $is->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

