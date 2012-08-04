#
# Filter command testing
#
# Filter can be set either by a "server adjunct command" in the login:
# user <username> pass <passcode> vers <soft> <vers> filter <filterstring>
#
# or with a #command in the incoming stream:
# "# filter <filterstring>"
# "#filter <filterstring>"
# "#alfhilawuehflwieuhffilter <filterstring>"
#
# Yes, the command can be prefixed with anything, even in the "user"
# command.
#

use Test;
BEGIN { plan tests => 6 + 1 + 2 + 2 + 2 + 3 };
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

# First filter is for uncompressed packet, second for compressed,
# third for mic-e, fourth for prefix filter test.
# The first and last one also test upper-case letters as filter keys.
my $i_rx = new Ham::APRS::IS("localhost:55580", "N5CAL-2",
	'filter' => 'p/OH');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests
my($tx, $rx, $helper);

# check that the initial p/ filter works
$tx = $rx = "OH2SRC>APRS,qAR,$login:>should pass";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# change filter
$i_rx->sendline("#filter p/N/K");
sleep(0.2);

# check that the new filter is applied
$tx = "OH2SRC>APRS,qAR,$login:>should drop";
$helper = "K2SRC>APRS,qAR,$login:>should pass";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

$tx = "OH2SRC>APRS,qAR,$login:>should drop2";
$helper = "N2SRC>APRS,qAR,$login:>should pass2";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# change filter, with some spaces
$i_rx->sendline("#  filter   p/N/K  ");
sleep(0.2);

# check that the new filter is applied
$tx = "OH2SRC>APRS,qAR,$login:>should drop3";
$helper = "K2SRC>APRS,qAR,$login:>should pass3";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

$tx = "OH2SRC>APRS,qAR,$login:>should drop4";
$helper = "N2SRC>APRS,qAR,$login:>should pass4";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# change filter, with some rubbish in front
$i_rx->sendline("#blaablaaanxauyg3iuyq2gfilter p/OG/OF/OH");
sleep(0.2);

# check that the new filter is applied
$tx = "K2SRC>APRS,qAR,$login:>should drop5";
$helper = "OH2SRC>APRS,qAR,$login:>should pass5";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

$tx = "N2SRC>APRS,qAR,$login:>should drop6";
$helper = "OH2SRC>APRS,qAR,$login:>should pass6";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

