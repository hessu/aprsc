#
# Miscellanious bad packets which should be dropped
#
# 1 - 3rd party packets }
# 2 - generic queries ?
# 3 - TCPIP, TCPXX, NOGATE, RFONLY in path
#

use Test;
BEGIN { plan tests => 8 + 1 + 5 };
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

my $i_rx = new Ham::APRS::IS("localhost:55152", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $unver_call = "N5UN-1";
my $i_un = new Ham::APRS::IS("localhost:55580", $unver_call, 'nopass' => 1);
ok(defined $i_un, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

$ret = $i_un->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_un->{'error'});

# do the actual tests
my($tx, $rx);

# Test that the unverified client does not manage to transmit anything
# at all.
$tx = "OH2XXX>APRS,qAR,$login:>should drop from unverified client";
$i_un->sendline($tx);
$tx = "$unver_call>APRS,qAR,$unver_call:>should drop from unverified client, 2";
$i_un->sendline($tx);
$tx = "$unver_call>APRS,qAR,$unver_call:!6028.52N/02505.61E# Testing";
$i_un->sendline($tx);

# Other drop reasons
my @pkts = (
	"SRC>APRS,RFONLY,qAR,$login:>should drop, RFONLY",
	"SRC>APRS,NOGATE,qAR,$login:>should drop, NOGATE",
	"SRC>APRS,RFONLY,qAR,$login:>should drop, RFONLY",
	"SRC>DST,DIGI,qAR,$login:}SRC2>DST,DIGI,TCPIP*:>should drop, 3rd party",
	"SRC>DST,DIGI,qAR,$login:}SRC3>DST,DIGI,TCPXX*:>should drop, 3rd party TCPXX",
	"SRC>DST,DIGI,qAR,$login:?APRS? general query",
	"SRC>DST,DIGI,qAR,$login:?WX? general query",
	"SRC>DST,DIGI,qAR,$login:?FOOBAR? general query",
	"SRC>DST\x08,DIGI,qAR,$login:>should drop ctrl-B in dstcall",
	"SRC\x08>DST,DIGI,qAR,$login:>should drop ctrl-B in srccall",
	" SRC>DST,DIGI,qAR,$login:>should drop, space in front of srccall",
	"\x00SRC>DST,DIGI,qAR,$login:>should drop, null in front of srccall",
	"SRCXXXXXXX>APRS,qAR,$login:>should drop, too long srccall",
	"SRC>APRSXXXXXX,qAR,$login:>should drop, too long dstcall",
	"SRC>APRS,OH2DIGI-12,qAR,$login:>should drop, too long call in path",
	"SRC>APT311,RELAY,WIDE,WIDE/V,qAR,$login:!4239.93N/08254.93Wv342/000 should drop, / in digi path",
	"SRC>DST,DIG*I,qAR,$login:>should drop, * in middle of digi call",
	"SRC>DST,DI\x08GI,qAR,$login:>should drop, ctrl-B in middle of digi call",
	"SRC2>DST,DIGI,qAX,$login:>Packet from unverified login according to qAX",
	"SRC2>DST,DIGI,TCPXX,qAR,$login:>Packet from unverified login according to TCPXX in path",
	"SRC2>DST,DIGI,TCPXX*,qAR,$login:>Packet from unverified login according to TCPXX* in path",
);

# send the packets
foreach my $s (@pkts) {
	$i_tx->sendline($s);
}

# check that a packet passes at all and the previous packets were dropped
$tx = "OH2SRC>APRS,OH2DIG-12*,OH2DIG-1*,qAR,200106F8020204020000000000000002,$login:}SRC2>DST:should pass";
$i_tx->sendline($tx);

my $fail = 0;
my $success = 0;
while (my $rx = $i_rx->getline_noncomment(0.5)) {
	if ($rx =~ /should pass/) {
		$success = 1; # ok
	} else {
		warn "Server passed packet it should have dropped: $rx\n";
		$fail++;
	}
}

ok($fail, 0, "Server passed packets which it should have dropped.");
ok($success, 1, "Server did not pass final packet which it should have passed.");

# disconnect

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

$ret = $i_un->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_un->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");



