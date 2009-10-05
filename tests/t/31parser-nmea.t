#
# Test NMEA packet parsing and filtering capability:
#
# verify that these packets pass based on a filter which matches
# the positions in these packets
#

use Test;
BEGIN { plan tests => 11 };
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

my $i_rx = new Ham::APRS::IS("localhost:14581", "N0CALL-2",
	'filter' => 'r/-38.5452/-58.7366/1 s/->' # GPRMC
	);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my($tx, $rx);

# 8: should pass filter by position
$tx = "OH1XYZ>GPSMW:\$GPRMC,184649,A,3832.7107,S,05844.1957,W,0.000,0.0,130909,4.5,W*62";
$rx = "OH1XYZ>GPSMW,qAS,$login:\$GPRMC,184649,A,3832.7107,S,05844.1957,W,0.000,0.0,130909,4.5,W*62";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# 9: should pass filter by car symbol
$tx = "OH2XYZ>GPSMV:\$GPRMC,212052,A,4609.1157,N,12258.8145,W,0.168,13.4,130909,17.9,E*6B";
$rx = "OH2XYZ>GPSMV,qAS,$login:\$GPRMC,212052,A,4609.1157,N,12258.8145,W,0.168,13.4,130909,17.9,E*6B";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# 10: should drop, wrong position and symbol
$tx = "OH3XYZ>GPSMW:\$GPRMC,212052,A,4609.1157,N,12258.8145,W,0.168,13.4,130909,17.9,E*6B";
my $dummy = "OH4XYZ>GPSMV:\$GPRMC,212052,A,4609.1157,N,12258.8145,W,0.168,13.4,130909,17.9,E*6B";
istest::should_drop(\&ok, $i_tx, $i_rx,
        $tx, # should drop
        $dummy); # will pass (helper packet)

# stop

ok($p->stop(), 1, "Failed to stop product");

