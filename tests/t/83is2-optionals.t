
#
# Test the forwarding of optional packet fields: RSSI, SNR.
#

use strict;
use Test;
BEGIN { plan tests => 8 + 3 + 4 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $server_call = "TESTING";

my $login_tx = "N0GATE";
my $i_tx = new Ham::APRS::IS2("localhost:56580", $login_tx);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS2");

my $login_tx_plain = "N0GATE-2";
my $i_plain_tx = new Ham::APRS::IS("localhost:55580", $login_tx_plain);
ok(defined $i_plain_tx, 1, "Failed to initialize Ham::APRS::IS");

# We set a filter on the rx so that the helper packets get through
my $login_rx = "N1GATE";
my $i_rx = new Ham::APRS::IS2("localhost:56580", $login_rx,
	'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS2");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_plain_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_plain_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $msg_src = "M1SRC";
my($tx, $rx, $tx_l, $rx_l);

$tx_l = "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:!6028.51N/02505.68E# should pass";
$tx = APRSIS2::ISPacket->new({
    'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
    'is_packet_data' => $tx_l,
    'rx_rssi' => 42,
    'rx_snr_db' => 12,
});
$rx_l = "$msg_src>APRS,OH2RDG*,WIDE,qAR,$login_tx:!6028.51N/02505.68E# should pass";
$rx = APRSIS2::ISPacket->new({
    'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
    'is_packet_data' => $rx_l,
    'rx_rssi' => 42,
    'rx_snr_db' => 12,
});
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx, 0, 1);

# transmit a position packet on the plain IS connection, with metadata
$tx = "$msg_src>APRS,OH2RDG*,WIDE,$login_tx_plain,I:!6028.51N/02505.68E# should pass 2";
$i_plain_tx->sendline("#is2-meta rx_rssi=42 rx_snr_db=12");
$rx_l = "$msg_src>APRS,OH2RDG*,WIDE,qAR,$login_tx_plain:!6028.51N/02505.68E# should pass 2";
$rx = APRSIS2::ISPacket->new({
    'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
    'is_packet_data' => $rx_l,
    'rx_rssi' => 42,
    'rx_snr_db' => 12,
});
istest::txrx(\&ok, $i_plain_tx, $i_rx, $tx, $rx, 0, 1);

# transmit a position packet on the plain IS connection, with metadata
$tx = "$msg_src>APRS,OH2RDG*,WIDE,$login_tx_plain,I:!6028.51N/02505.68E# should pass 3";
$i_plain_tx->sendline("#is2-meta rx_snr_db=12");
$rx_l = "$msg_src>APRS,OH2RDG*,WIDE,qAR,$login_tx_plain:!6028.51N/02505.68E# should pass 3";
$rx = APRSIS2::ISPacket->new({
    'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
    'is_packet_data' => $rx_l,
    'rx_snr_db' => 12,
});
istest::txrx(\&ok, $i_plain_tx, $i_rx, $tx, $rx, 0, 1);

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_plain_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_plain_tx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

