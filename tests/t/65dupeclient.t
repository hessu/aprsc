#
# Test client receiving dupes
#

use Test;
BEGIN { plan tests => 2 + 4*2 + 4 + 4 + 1 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS2;

# initialize and start the product up
my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# create connections to the server:
# - i_tx for transmitting packets
# - i_rx for receiving packets
# - i_dup for receiving dupes
# - i_dup_is2 for receiving dupes using IS2

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:55152", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $i_dup = new Ham::APRS::IS("localhost:55153", "DUPS");
ok(defined $i_dup, 1, "Failed to initialize Ham::APRS::IS");

my $i_dup_is2 = new Ham::APRS::IS2("localhost:56153", "DUPIS2");
ok(defined $i_dup_is2, 1, "Failed to initialize Ham::APRS::IS2");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server filter port: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server full feed: " . $i_rx->{'error'});
$ret = $i_dup->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server dupe port: " . $i_dup->{'error'});
$ret = $i_dup_is2->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server IS2 dupe port: " . $i_dup->{'error'});

# do the actual tests

my $data = 'foo';

# Send an unique packet
#
# istest::txrx( $okref, $transmit_connection, $receive_connection,
#	$transmitted_line, $expected_line_on_rx );
#
my $l = "SRC>DST,qAR,$login:$data";
istest::txrx(\&ok, $i_tx, $i_rx, $l, $l);

# Send again, and check that it only comes out on the dupe socket
istest::should_drop(\&ok, $i_tx, $i_rx,
	$l, # should drop
	"SRC>DST:dummy", 1); # will pass (helper packet)

my $d = $i_dup->getline_noncomment();
ok($d, "dup\t$l", "Got wrong line on dupe socket");

my $d2 = $i_dup_is2->get_packet(5, 1);
ok($d2, "$l", "Got wrong line on IS2 dupe socket: " . $i_dup_is2->{'error'});

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});
$ret = $i_dup->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_dup->{'error'});
$ret = $i_dup_is2->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_dup_is2->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

