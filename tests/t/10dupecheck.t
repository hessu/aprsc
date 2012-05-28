#
# Test duplicate detection capability
#

use Test;
BEGIN { plan tests => 17 };
use runproduct;
use istest;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

# initialize and start the product up
my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# create two connections to the server:
# - i_tx for transmitting packets
# - i_rx for receiving packets

my $login = "N0CALL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:55152", "N0CALL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $data = 'foo';

# First, send a non-APRS packet and see that it goes through.
#
# istest::txrx( $okref, $transmit_connection, $receive_connection,
#	$transmitted_line, $expected_line_on_rx );
#
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,qAR:$data",
	"SRC>DST,qAS,$login:$data");

# Send the same packet again immediately and see that it is dropped.
# The 1 in as the 6th parameter means that the dummy helper packet is
# also transmitted after the second packet, and that it is the only
# packet which should come out.
istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRC>DST,qAR:$data", # should drop
	"SRC>DST:dummy", 1); # will pass (helper packet)

# source callsign should be case sensitive - verify that a lower-case
# callsign gets through
istest::txrx(\&ok, $i_tx, $i_rx,
	"src>DST,qAR:$data",
	"src>DST,qAS,$login:$data");

# send the same packet with a different digi path and see that it is dropped
istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1*,qAR:$data", # should drop
	"SRC>DST:dummy", 1); # will pass (helper packet)

# send the same packet with a different Q construct and see that it is dropped
istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRC>DST,$login,I:$data", # should drop
	"SRC>DST:dummy", 1); # will pass (helper packet)

# send the same packet with additional whitespace in the end, should pass umodified
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,$login,I:$data  ",
	"SRC>DST,qAR,$login:$data  ");

# send the same packet with a different destination call, should pass
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST2,DIGI1*,qAR:$data",
	"SRC>DST2,DIGI1*,qAS,$login:$data");

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

