#
# Test negative filters
#

use Test;
BEGIN { plan tests => 10 };
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

# allow range, then drop using a buddy filter
my $i_rx = new Ham::APRS::IS("localhost:55581", "N5CAL-2",
	'filter' => 'r/60.4752/25.0947/100 -b/D3NY');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $srccall = "OH2RDP-1";
my $dstcall = "BEACON-15";
my($tx, $rx, $helper);

# check that the r/ range filter passes packets within the range
$tx = "$srccall>$dstcall,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass";
$rx = "$srccall>$dstcall,OH2RDG*,WIDE,qAS,$login:!6028.51N/02505.68E# should pass";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

