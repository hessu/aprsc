#
# Test APRS packet parsing and filtering capability:
# - basic uncompressed packet
# - basic compressed packet
# - basic mic-e packet
# verify that these packets pass based on a filter which matches
# the positions in these packets, and also quickly try out the
# p/ prefix filter
#

use Test;
BEGIN { plan tests => 17 };
use runproduct;
use istest;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N0CALL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:14580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

# first filter is for uncompressed packet, second for compressed,
# third for mic-e, fourth for prefix filter test
my $i_rx = new Ham::APRS::IS("localhost:14581", "N0CALL-2",
	'filter' => 'r/60.4752/25.0947/1 r/60.0520/24.5045/1 r/37.0887/-76.4585/100 p/OG/OF3/N');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $phg = "7220";
my $srccall = "OH2RDP-1";
my $dstcall = "BEACON-15";
my($tx, $rx, $helper);

# UNCOMPRESSED packet
# check that the r/ range filter passes packets within the range
$tx = "$srccall>$dstcall,OH2RDG*,WIDE:!6028.51N/02505.68E#PHG$phg should pass";
$rx = "$srccall>$dstcall,OH2RDG*,WIDE,qAS,$login:!6028.51N/02505.68E#PHG$phg should pass";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that the r/ range filter drops packets outside the range
$tx = "$srccall>$dstcall,OH2RDG*,WIDE:!6258.51N/02505.68E#PHG$phg should drop";
$helper = "$srccall>$dstcall,OH2RDG*,WIDE:!6028.51N/02505.68E#PHG$phg should pass";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# COMPRESSED packet
$tx = "$srccall>$dstcall:!I0-X;T_Wv&{-Aigate testing";
$rx = "$srccall>$dstcall,qAS,$login:!I0-X;T_Wv&{-Aigate testing";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# mic-e packet
$srccall = "N3HF-9";
$tx = "$srccall>S7PU3R:`h7Oq+F>/`\"3{}_";
$rx = "$srccall>S7PU3R,qAS,$login:`h7Oq+F>/`\"3{}_";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that the P filter passes packets with the prefixes given
$tx = "OG7LZB>$dstcall,OH2RDG*,WIDE:!6028.51N/02505.68E#PHG$phg should pass";
$rx = "OG7LZB>$dstcall,OH2RDG*,WIDE,qAS,$login:!6028.51N/02505.68E#PHG$phg should pass";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that the P filter passes packets with the prefixes given
$tx = "OF3LZB>$dstcall,OH2RDG*,WIDE:!5028.51S/02505.68E#PHG$phg should pass";
$rx = "OF3LZB>$dstcall,OH2RDG*,WIDE,qAS,$login:!5028.51S/02505.68E#PHG$phg should pass";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that the P filter passes packets with the prefixes given
$tx = "N1CAL>$dstcall,OH2RDG*,WIDE:!5028.51S/02505.68E#PHG$phg should pass";
$rx = "N1CAL>$dstcall,OH2RDG*,WIDE,qAS,$login:!5028.51S/02505.68E#PHG$phg should pass";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);


# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

