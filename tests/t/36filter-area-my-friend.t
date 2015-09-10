#
# Test filters for area (a/), my position range (m/), friend pos range (f/)
#

use Test;
BEGIN { plan tests => 6 + 4 + 2 + 6 + 1 };
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

# allow range, then drop using a buddy filter
my $i_rx = new Ham::APRS::IS("localhost:55581", $rxlogin, 'nopass' => 1);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

my($tx, $rx, $helper);

###############################################
# filter for area in north-east
$i_rx->sendline("#filter a/60.1874/24.8362/60.1872/24.8365");
sleep(0.5);

$pass = "$login>APRX1M,qAR,$login:/054048h6011.24N/02450.18E_c212s003g006t053h82b10046";
$drop = "DR0P>APRX1M,qAR,$login:/054048h6011.30N/02450.18E_c212s003g006t053h82b10046";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

$pass = "$login>RXTLM-1,qAR,$login:T#363,12.2,0.0,62.0,13.0,0.0,00000000";
$drop = "DR0P>APRX1M,qAR,$login:/054048h6011.224N/02450.28E_c212s003g006t053h82b10046";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# filter for area in south-west
$i_rx->sendline("#filter a/-23.5679/-47.3288/-23.5688/-47.3278");
sleep(0.5);

$pass = "PU2TFG-15>APN390,qAR,PY2PE-1:!2334.10S/04719.70W# pass";
$drop = "DR0P-15>APN390,qAR,PY2PE-1:!2334.10N/04719.70W# drop north";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

$pass = "PU2TFG-15>BEACON,qAR,PY2PE-1:Rede Brasileira de APRS - Aluminio/SP";
$drop = "DR0P-15>APN390,qAR,PY2PE-1:!2334.10S/04719.70E# drop east";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

###############################################
# filter for range around my position, and transmit my position too
# also set a filter for friend range (it's position is already known
# from previous test)
$i_rx->sendline("#filter m/1 f/$login/10");
$i_rx->sendline("$rxlogin>APRS:!//HC`TPmVvR<_"); # tampere
sleep(0.5);

$pass = "T3ST>APRS,qAR,$login:!//H:sTQ\";vJI_ test"; # close to the previous compressed
$drop = "DR0P-15>APN390,qAR,$login:!2334.10S/04719.70E#";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# OH2TI is close enough to OH2TI, OH2FOI is not.
$pass = "OH1UK>APRS,TCPIP*,qAC,T2FINLAND:;OH2TA    *101302z6014.89N/02447.00Ey";
$drop = "OH2FOI>APRS,qAR,$login:!6013.90N/02500.05E-";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

###############################################
# reconnect and see if the m/ filter still works based
# on a cached position in historydb
$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to reconnect to the server: " . $i_tx->{'error'});
$i_tx->sendline("#filter m/1");
sleep(0.5);

# new transmitter needed
$tx2_login = 'TX2LOG';
my $i_tx2 = new Ham::APRS::IS("localhost:55581", $tx2_login);
ok(defined $i_tx2, 1, "Failed to initialize Ham::APRS::IS");
$ret = $i_tx2->connect('retryuntil' => 8);
ok($ret, 1, "Failed to reconnect to the server: " . $i_tx2->{'error'});

$pass = "T3ST-1>APRS,qAR,$tx2_login:/054048h6011.24N/02450.18E_c212s003g006t053h82b10046";
$drop = "DR0P-15>APN390,qAR,$tx2_login:!2334.10S/04719.70E#";
istest::should_drop(\&ok, $i_tx2, $i_tx, $drop, $pass);

# stop

ok($p->stop(), 1, "Failed to stop product");

