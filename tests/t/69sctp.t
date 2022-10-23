
#
# Test SCTP, only on aprsc
#

use Test;

BEGIN {
	plan tests => (!defined $ENV{'TEST_PRODUCT'} || $ENV{'TEST_PRODUCT'} =~ /aprsc/) ? 4 + 1 + 4 + 3 + 2 : 0;
};

if (defined $ENV{'TEST_PRODUCT'} && $ENV{'TEST_PRODUCT'} !~ /aprsc/) {
	exit(0);
}

use runproduct;
use LWP;
use LWP::UserAgent;
use HTTP::Request::Common;
use JSON::XS;
use Ham::APRS::IS;
use istest;
use Data::Dumper;

# set up the JSON module
my $json = new JSON::XS;

if (!$json) {
	die "JSON loading failed";
}

$json->latin1(0);
$json->ascii(1);
$json->utf8(0);

# set up http client ############

my $ua = LWP::UserAgent->new;

$ua->agent(
	agent => "httpaprstester/1.0",
	timeout => 10,
	max_redirect => 0,
);

# set up two aprsc processes, the leaf connects to the hub with SCTP
my $phub = new runproduct('sctp-hub', 'hub');
my $pleaf = new runproduct('sctp-leaf', 'leaf');

ok(defined $phub, 1, "Failed to initialize product runner");
ok(defined $pleaf, 1, "Failed to initialize product runner");
ok($phub->start(), 1, "Failed to start hub");
ok($pleaf->start(), 1, "Failed to start leaf");

# wait for the leaf to connect to the hub
my $timeout_at = time() + 10;

my $uplink;

while (time() < $timeout_at) {
	# first get the status page without any traffic
	$res = $ua->simple_request(HTTP::Request::Common::GET("http://127.0.0.1:55502/status.json"));
	if ($res->code ne 200) {
		continue;
	}
	my $j1 = $json->decode($res->decoded_content(charset => 'none'));
	if (!defined $j1) {
		continue;
	}
	if (!defined $j1->{'uplinks'} || length($j1->{'uplinks'}) < 1) {
		continue;
	}
	$uplink = $j1->{'uplinks'}->[0];
	if ($uplink->{'username'} eq 'TESTING-1') {
		last;
	}
	undef $uplink;
	sleep(1);
}

ok(defined $uplink, 1, "Leaf server failed to establish an SCTP uplink with hub");

# establish connections

my $login_tx = "N0GAT";
my $i_tx = new Ham::APRS::IS("localhost:55583", $login_tx);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $login_rx = "N1GAT";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login_rx);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

############################################

my $flush_interval = 300;
my $bytelimit = 4*1024*1024;
my $window = 64*1024;
#my $window = 2*1024;
my $outstanding = 0;
my $txn = 0;
my $rxn = 0;
my $txl = 0;
my $rxl = 0;
my @l = ();
my $txq = '';
my $txq_l = 0;

my $start_t = time();

while ($txl < $bytelimit) {
	$s = "M" . ($txn % 10000 + 10) . ">APRS,qAR,$login_tx:!6028.51N/02505.68E# packet $txn blaa blaa blaa blaa END";
	push @l, $s;
	$s .= "\r\n";
	my $sl = length($s);
	$txl += $sl;
	$txq_l += $sl;
	$txq .= $s;
	$txn++;
	
	if ($txq_l >= $flush_interval) {
		$i_tx->sendline($txq, 1);
		$outstanding += $txq_l;
		$txq_l = 0;
		$txq = '';
	}
	
	while (($outstanding > $window) && (my $rx = $i_rx->getline_noncomment(1))) {
		my $exp = shift @l;
		if ($exp ne $rx) {
			warn "Ouch, received wrong packet: $rx\nExpected: $exp";
		}
		my $rx_l = length($rx) + 2;
		$outstanding -= $rx_l;
		$rxn++;
		$rxl += $rx_l;
	}
}

if ($txq_l > 0) {
	warn "flushing the rest\n";
	$i_tx->sendline($txq, 1);
	$outstanding += $txq_l;
	$txq_l = 0;
	$txq = 0;
}

warn "reading the rest, have received $rxn packets, sent $txn\n";
while (($outstanding > 0) && (my $rx = $i_rx->getline_noncomment(2))) {
	my $exp = shift @l;
	if ($exp ne $rx) {
		warn "Ouch, received wrong packet: $rx\n";
	}
	my $rx_l = length($rx) + 2;
	$outstanding -= $rx_l;
	$rxn++;
	$rxl += $rx_l;
}
warn "after reading the rest, have received $rxn packets, sent $txn, outstanding $outstanding bytes\n";

$end_t = time();
$dur_t = $end_t - $start_t;

warn sprintf("took %.3f seconds, %.0f packets/sec\n", $dur_t, $rxn / $dur_t);

ok($rxn, $txn, "Received wrong number of lines from blob");
ok($rxl, $txl, "Received wrong number of bytes from blob");
ok($outstanding, 0, "There are outstanding bytes in the server after timeout");

# stop

ok($phub->stop(), 1, "Failed to stop hub");
ok($pleaf->stop(), 1, "Failed to stop leaf");


