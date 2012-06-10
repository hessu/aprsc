#
# Test packet type and symbol filters
#

use Test;
BEGIN { plan tests => 6 + 10 + 3 };
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

# set a filter for half of the types
$i_rx->sendline("#filter t/poimq -t/nw");
sleep(0.5);

# status drop, position pass
$drop = "ST1>APRS,qAR,$login:>status type drop";
$pass = "ST2>APRS,qAR,$login:!6028.51N/02505.68E# type pos";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# object pass, telemetry drop
$drop = "ST3>APRS,qAR,$login:T#931,113,000,000,000,000,00000000";
$pass = "ST4>APRS,qAR,$login:;OBJ1     *090902z6010.78N/02451.11E-Object 1";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# item pass, User-defined drop
$drop = "ST5>APRS,qAR,$login:{Q1qwerty";
$pass = "ST6>APRS,qAR,$login:)OBJ1!4903.50N/07201.75WA";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# message pass, NWS object drop (object, not message)?
# Couldn't find an example
#$drop = sprintf("ST7>APRS,qAR,%s::%-9.9s:NWS message{ab", $login, 'NWS-ADVIS');
#$drop = "ST8>APRS,qAR,$login:;OBJ1     *090902z6010.78N/02451.11E-Object 1";
$tx = $rx = sprintf("ST7>APRS,qAR,%s::%-9.9s:normal message{ac", $login, 'DSTC');
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# query message pass, wx drop.
# wx must be explicitly dropped with -t/w since the wx packet also has a position
# and would match t/p.
# Will also pass the message filter, so better test separately
# with a filter that does not pass messages.
$pass = sprintf("ST9>APRS,qAR,%s::%-9.9s:?APRSD", $login, 'IGCALL');
$drop = "ST10>APRS,qAR,$login:\@100857z5241.73N/00611.14E_086/002g008t064r000p000P000h63b10102L810.DsIP-VP";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# set a filter for second half of the types
$i_rx->sendline("#filter t/stunwq");
sleep(0.5);

# status pass, position drop
$pass = "ST11>APRS,qAR,$login:>status pass";
$drop = "ST12>APRS,qAR,$login:!6028.51N/02505.68E# pos drop";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# object drop, telemetry pass
$pass = "ST13>APRS,qAR,$login:T#931,113,000,000,000,000,00000000";
$drop = "ST14>APRS,qAR,$login:;OBJ1     *090902z6010.78N/02451.11E-Object 1";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# item drop, User-defined pass
$pass = "ST15>APRS,qAR,$login:{Q1qwerty";
$drop = "ST16>APRS,qAR,$login:)OBJ1!4903.50N/07201.75WA";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# message drop, weather pass
$drop = sprintf("ST17>APRS,qAR,%s::%-9.9s:normal message again{ff", $login, 'DSTC');
$pass = "ST18>APRS,qAR,$login:\@100857z5241.73N/00611.14E_086/002g008t064r000p000P000h63b10102L810.DsIP-VP";
istest::should_drop(\&ok, $i_tx, $i_rx, $drop, $pass);

# query pass
$tx = $rx = sprintf("ST19>APRS,qAR,%s::%-9.9s:?APRSP", $login, 'IGCALL');
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);


# distance in type filter
$i_rx->sendline("#filter t/w/POSIT/10");
sleep(0.5);




# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

