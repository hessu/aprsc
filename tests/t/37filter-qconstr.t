#
# Test filters for area (a/), my position range (m/), friend pos range (f/)
#

use Test;
BEGIN { plan tests => 6 + 4 + 1 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Time::HiRes qw(sleep);

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $rxlogin = "N5CAL-2";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:55581", $rxlogin);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

my($tx, $rx, $helper);

###############################

# filter for Q construct
$i_rx->sendline("#filter q/RUo");
sleep(0.5);

$pass = "PU2TFG-15>APN390,qAR,$login:!2334.10S/04719.70W# pass qAR";
$drop = "$login>APN390:!2334.10N/04719.70W# drop qAC";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# this drop is outside documentation, but it is dropped
$pass = "PU2TFG-1>APN390,qAU,$login:!2334.10S/04719.70W# pass qAU";
$drop = "$login>APN390,qAU,$login:!2334.10N/04719.70W# drop qAU)";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

$pass = "PU2TFG-2>APN390,qAo,SRVR1:!2334.10S/04719.70W# pass qAo";
$drop = "DR0P>APN39X,qAr,CL1ENT:!2334.10N/04719.70W# drop qAr)";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

$pass = "PU2TFG-2>APN390,qAo,SRVR1:!2334.10S/04719.70W# pass qAo";
$drop = "DR0P-2>APN39,qAO,CL1ENT:!2334.10N/04719.70W# drop qAr)";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# stop

ok($p->stop(), 1, "Failed to stop product");

