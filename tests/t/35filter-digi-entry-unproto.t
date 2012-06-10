#
# Test filters for digipeater, entrycall and unproto
#

use Test;
BEGIN { plan tests => 6 + 4 + 2 + 2 + 1 };
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

my($tx, $rx, $helper);

# filter for digi callsign
$i_rx->sendline("#filter d/OH7RDA/OH2RDS/OH9*");
sleep(0.5);

# The filter must only match when the path element is USED*
$pass = "SRC1>APRS,OH7RDA*,qAR,$login:!6016.66NT02515.26E# pass digi1";
$drop = "SRC2>APRS,OH7RDA,qAR,$login:!6016.66NT02515.26E# pass digi1 unused";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# The * in a later digi also marks the previous ones as used.
$pass = "SRC3>APRS,OH7RDA,OH8RDA*,qAR,$login:!6016.66NT02515.26E# pass digi2";
$drop = "SRC4>APRS,OH7RDA,OH8RDA,qAR,$login:!6016.66NT02515.26E# pass digi2 unused";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# do the same with a wildcarded filter entry
$pass = "SRC5>APRS,OH9RDA*,qAR,$login:!6016.66NT02515.26E# pass digi1";
$drop = "SRC6>APRS,OH9RDA,qAR,$login:!6016.66NT02515.26E# pass digi1 unused";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

$pass = "SRC7>APRS,OH9RDA,OH8RDA*,qAR,$login:!6016.66NT02515.26E# pass digi2";
$drop = "SRC8>APRS,OH9RDA,OH8RDA,qAR,$login:!6016.66NT02515.26E# pass digi2 unused";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# filter for q construct entrycall
$i_rx->sendline("#filter e/$login/IGA*");
sleep(0.5);

$pass = "ESRC1>APRS,qAR,$login:!6016.66NT02515.26E# pass entrycall";
$drop = "ESRC2>APRS,qAR,BAA:!6016.66NT02515.26E# drop entrycall";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# try with wildcard, and the matching call as non-first entry
$pass = "ESRC3>APRS,qAR,IGATE1:!6016.66NT02515.26E# pass entrycall wildcard";
$drop = "ESRC4>APRS,qAR,FOO,IGATE2:!6016.66NT02515.26E# drop entrycall wildcard";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# filter for unproto address
$i_rx->sendline("#filter u/ALTNET/APZ*");
sleep(0.5);

$pass = "USRC1>ALTNET,qAR,$login:!6016.66NT02515.26E# pass unproto";
$drop = "USRC2>ALTBLA,qAR,$login:!6016.66NT02515.26E# drop unproto";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

$pass = "USRC3>APZMDR,qAR,$login:!6016.66NT02515.26E# pass unproto wildcard";
$drop = "USRC4>APXMDR,qAR,$login:!6016.66NT02515.26E# drop unproto wildcard";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);


# stop

ok($p->stop(), 1, "Failed to stop product");

