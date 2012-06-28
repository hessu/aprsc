
#
# Test heavier packet load
#

use Test;
BEGIN { plan tests => 10 };
use runproduct;
use Ham::APRS::IS;
use Time::HiRes qw( sleep time );

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login_tx = "N0GAT";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login_tx);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $login_rx = "N1GAT";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login_rx);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

# let it get started
sleep(0.5);

############################################

my $flush_interval = 300;
my $bytelimit = 4*1024*1024;
my $window = 64*1024;
#my $window = 4*1024;
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
	
	while (($outstanding > $window) && (my $rx = $i_rx->getline_noncomment(0))) {
		my $exp = shift @l;
		if ($exp ne $rx) {
			warn "Ouch, received wrong packet: $rx\n";
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
while (($outstanding > 0) && (my $rx = $i_rx->getline_noncomment(0.5))) {
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

ok($p->stop(), 1, "Failed to stop product");

