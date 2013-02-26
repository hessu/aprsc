#
# Some bad packets which should be allowed in quirks mode
# (client has known bad-and-unmaintained software)
#

use Test;
BEGIN { plan tests => 6 + 3 + 3 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Time::HiRes qw(sleep);

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

$Ham::APRS::IS::aprs_appid = 'HR-IXPWINDi-123';

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:55152", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests
my($tx, $rx);

# spaces in front of srccall
$rx = "SRC>DST,qAR,$login:>spaces in beginning";
$tx = "   $rx";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# nulls in front of srccall
$rx = "SRC>DST,qAR,$login:>nulls in beginning";
$tx = "\x00$rx";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# spaces in end of srccall
my $tail = ">DST,qAR,$login:>spaces in end of srccall";
$tx = "SRC  $tail";
$rx = "SRC$tail";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# disconnect

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");



