#
# Test 3rd-party packet header parsing and filtering
#

use Test;
BEGIN { plan tests => 6 + 9 + 3 };
use runproduct;
use istest;
use Ham::APRS::IS;

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

# First filter is for uncompressed packet, second for compressed,
# third for mic-e, fourth for prefix filter test.
# The first and last one also test upper-case letters as filter keys.
my $i_rx = new Ham::APRS::IS("localhost:55581", "N5CAL-2",
	'filter' => 'r/60.228/24.8495/5 p/OG p/OI p/K b/BB7LZB');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

############### tests ########################

# check that 3rd party packet matches a P filter
$tx = $rx = "UU0AA>TEST,qAR,IGATE:}OG7LZB>DST,NET,GATE*:>should pass 3rd party inner prefix";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that 3rd party packet matches a B filter
$tx = $rx = "UU0AA>TEST,qAR,IGATE:}BB7LZB>DST,NET,GATE*:>should pass 3rd party inner prefix with b filter";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that 3rd party packet matches a P filter
$tx = $rx = "OI0AA>TEST,qAR,IGATE:}ZZ7LZB>DST,NET,GATE*:>should pass 3rd party outer prefix";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that 3rd party packet matches a coordinate filter
$tx = $rx = "UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:!6013.69NR02450.97E&";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that a nested 3rd party packet matches a coordinate filter
$tx = $rx = "UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:}OF7LZC>DST,NET2,GATE2:!6013.69NR02450.97E&";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that a really nested 3rd party packet matches a coordinate filter
$tx = $rx = "UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:}OF7LZC>DST,NET2,GATE2:}OF7LZD>DST,NET2,GATE2:}OF7LZE>DST,NET2,GATE2:!6013.69NR02450.97E&";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# Invalid 3rd-party header, does not have 2 hops
$tx = "KG4LAA>APWW08,TCPIP*,qAS,n3wax:}KB3ONM>APK102,KV3B-1,WIDE1,WIDE2,KV3B-1,WIDE1,KG4LAA*::N3HEV-9  :ack26";
$helper = "K2SRC>APRS,qAR,$login:>should pass 1";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# Invalid 3rd-party header, does not have 2 hops
$tx = "KG4LAA>APWW08,TCPIP*,qAS,n3wax:}KB3ONM>APK102,KV3B-1::N3HEV-9  :ack26";
$helper = "K2SRC>APRS,qAR,$login:>should pass 2";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# Invalid 3rd-party header, only has the } marker, but is passed still
# (needs to have }...>...: to be treated as 3rd-party)
$tx = "K3SRC>APWW08,TCPIP*,qAR,IGA:}blah blah";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $tx);

# disconnect #################################

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

