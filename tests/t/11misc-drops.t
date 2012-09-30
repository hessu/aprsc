#
# Miscellanious bad packets which should be dropped
#
# 1 - 3rd party packets }
# 2 - generic queries ?
# 3 - TCPIP, TCPXX, NOGATE, RFONLY in path
#

use Test;
BEGIN { plan tests => 6 + 1 + 3 };
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
my $i_rx = new Ham::APRS::IS("localhost:55152", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests
my($tx, $rx);

my @pkts = (
	"SRC>APRS,NOGATE,qAR,$login:>should drop, NOGATE",
	"SRC>APRS,RFONLY,qAR,$login:>should drop, RFONLY",
	"SRC>DST,DIGI,qAR,$login:}SRC2>DST,DIGI,TCPIP*:>should drop, 3rd party",
	"SRC>DST,DIGI,qAR,$login:}blah, 3rd party ID only",
	"SRC>DST,DIGI,qAR,$login:?APRS? general query",
	"SRC>DST,DIGI,qAR,$login:?WX? general query",
	"SRC>DST,DIGI,qAR,$login:?FOOBAR? general query"
);

# send the packets
foreach my $s (@pkts) {
	$i_tx->sendline($s);
}

# check that the initial p/ filter works
$tx = "OH2SRC>APRS,qAR,$login:>should pass";
$i_tx->sendline($tx);

my $fail = 0;

while (my $rx = $i_rx->getline_noncomment(0.5)) {
	if ($rx =~ /should pass/) {
		last;
	} else {
		warn "Server passed packet it should have dropped: $rx\n";
		$fail++;
	}
}

ok($fail, 0, "Server passed packets which it should have dropped.");

# disconnect

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");



