#
# Test more filter tipes than the ranges
#

use Test;
BEGIN { plan tests => 6 + 3 + 5 + 5 + 3 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Time::HiRes qw(sleep);

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

# allow range, then drop using a buddy filter
my $i_rx = new Ham::APRS::IS("localhost:55581", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# set a filter for prefix
$i_rx->sendline("#filter p/OH/G");
my($tx, $rx, $helper);

# let the filter command go through - it doesn't send any reply that
# we could match to
sleep(0.5);

$tx = "OH0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass prefix filter";
$rx = "OH0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass prefix filter";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

$tx = "G0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass prefix filter";
$rx = "G0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass prefix filter";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

$tx = "N0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should drop prefix filter";
$helper = "G0TES-2>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass prefix filter";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

############################
# set a buddy filter
$i_rx->sendline("#filter b/OH0TES/OH2TES b/OH7* b/OH*DA");
sleep(0.5);

# see that the filter does match
$tx = "OH0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass buddy filter";
$rx = "OH0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass buddy filter";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# the previously set prefix filter should no longer pass
$tx = "G0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should drop buddy filter";
$helper = "OH0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# helper pass";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# verify that the buddy filter does not act like a prefix filter
$tx = "OH0TES-9>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should drop buddy filter";
$helper = "OH0TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# helper pass2";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# wildcard in end
$tx = "OH7TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass wildcard buddy filter";
$rx = "OH7TES>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# should pass wildcard buddy filter";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# wildcard in middle
$tx = "OH9RDA>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# wildcard-middle buddy filter";
$rx = "OH9RDA>APRS,OH2RDG*,WIDE,qAR,$login:!6028.51N/02505.68E# wildcard-middle buddy filter";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

############################
# set an object filter
$i_rx->sendline("#filter o/OBJ1/OBJ2 o/PRE*/*END/FO*AR");
sleep(0.5);

# see that the filter does match
$tx = "SRC>APRS,qAR,$login:;OBJ1     *090902z6010.78N/02451.11E-Object 1";
$rx = "SRC>APRS,qAR,$login:;OBJ1     *090902z6010.78N/02451.11E-Object 1";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

$tx = "SRC>APRS,qAR,$login:;OBJ2     *090902z6010.78N/02451.11E-Object 2";
$rx = "SRC>APRS,qAR,$login:;OBJ2     *090902z6010.78N/02451.11E-Object 2";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# wildcard in end
$tx = "SRC>APRS,qAR,$login:;PREFIX   *090902z6010.78N/02451.11E-Object prefix";
$rx = "SRC>APRS,qAR,$login:;PREFIX   *090902z6010.78N/02451.11E-Object prefix";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# wildcard in beginning
$tx = "SRC>APRS,qAR,$login:;TEEND    *090902z6010.78N/02451.11E-Object suffix";
$rx = "SRC>APRS,qAR,$login:;TEEND    *090902z6010.78N/02451.11E-Object suffix";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# wildcard in middle
$tx = "SRC>APRS,qAR,$login:;FOOBAR   *090902z6010.78N/02451.11E-Object wild middle";
$rx = "SRC>APRS,qAR,$login:;FOOBAR   *090902z6010.78N/02451.11E-Object wild middle";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

