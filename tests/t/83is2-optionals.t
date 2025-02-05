
#
# Test the forwarding of optional packet fields: RSSI, SNR.
#

use strict;
use Test;
BEGIN { plan tests => 6 + 1 + 3 };
use runproduct;
use istest;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $server_call = "TESTING";

my $login_tx = "N0GATE";
my $i_tx = new Ham::APRS::IS2("localhost:56580", $login_tx,
	'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS2");

# We set a filter on the rx so that the helper packets get through
my $login_rx = "N1GATE";
my $i_rx = new Ham::APRS::IS2("localhost:56580", $login_rx,
	'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS2");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $msg_src = "M1SRC";
my $msg_dst = "M1DST";
my($tx, $rx, $tx_l, $rx_l);

# now, transmit a position packet on the receiving filtered port
$tx_l = "$msg_dst>APRS,OH2RDG*,WIDE,$login_rx,I:!6028.51N/02505.68E# should pass";
$tx = APRSIS2::ISPacket->new({
    'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
    'is_packet_data' => $tx_l,
    'rx_rssi' => 42,
    'rx_snr_db' => 12,
});
$rx_l = "$msg_dst>APRS,OH2RDG*,WIDE,qAR,$login_rx:!6028.51N/02505.68E# should pass";
$rx = APRSIS2::ISPacket->new({
    'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
    'is_packet_data' => $rx_l,
    'rx_rssi' => 42,
    'rx_snr_db' => 12,
});
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx, 0, 1);

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

