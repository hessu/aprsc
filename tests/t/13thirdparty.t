#
# Test 3rd-party packet header parsing and filtering
#

use Test;
BEGIN { plan tests => 6 + 3 + 3 };
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
	'filter' => 'r/60.228/24.8495/5 p/OG p/OI');
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

# check that 3rd party packet matches a P filter
$tx = $rx = "OI0AA>TEST,qAR,IGATE:}ZZ7LZB>DST,NET,GATE*:>should pass 3rd party outer prefix";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# check that 3rd party packet matches a coordinate filter
$tx = $rx = "UU1AA>TEST,qAR,IGATE:}OF7LZB>DST,NET,GATE:!6013.69NR02450.97E&";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);


# disconnect #################################

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

