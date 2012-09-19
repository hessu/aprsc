
#
# Test UDP core peers with a load chunk.
#

use Test;
BEGIN { plan tests => 6 + 2*3 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS_Fake_UDP;
use Time::HiRes qw(sleep time);

my $p = new runproduct('basic');

# UDP peer socket
my $udp = new Ham::APRS::IS_Fake_UDP('127.0.0.1:16405', 'N0UDP');
ok(defined $udp, (1), "Failed to set up UDP server socket");
ok($udp->bind_and_listen(), 1, "Failed to bind UDP server socket");
$udp->set_destination('127.0.0.1:16404');

# Start software
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $server_call = "TESTING";

# Set up client and connect
my $login_tx = "N5CAL-1";
my $i_full = new Ham::APRS::IS("localhost:55152", $login_tx);
ok(defined $i_full, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_full->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_full->{'error'});

# test ##########################

my $flush_interval = 500;
#$flush_interval = 300;
my $bytelimit = 1*1024*1024;
#my $bytelimit = 4096;
my $window = 24*1024;
#my $max_speed = 500; # packets /s

sub load_test($$$)
{
	my($prefix, $is_tx, $is_rx) = @_;
	
	my $outstanding = 0;
	my $txn = 0;
	my $rxn = 0;
	my $txl = 0;
	my $rxl = 0;
	my $txq = '';
	my $txq_l = 0;
	my %expected;
	
	my $start_t = time();
	
	while ($txl < $bytelimit) {
		$s = $prefix . ($txn % 10000 + 10) . ">APRS,qAR,OH9XYZ-5:!6028.51N/02505.68E# packet $txn blaa blaa blaa blaa END";
		$expected{$s} = 1;
		$s .= "\r\n";
		my $sl = length($s);
		$txl += $sl;
		$txq_l += $sl;
		$txq .= $s;
		$txn++;
		
		if ($txq_l >= $flush_interval || 1) {
			$is_tx->sendline($txq, 1);
			$outstanding += $txq_l;
			$txq_l = 0;
			$txq = '';
		}
		
		while (($outstanding > $window) && (my $rx = $is_rx->getline(1))) {
			next if ($rx =~ /^#/);
			if (!defined $expected{$rx}) {
				die "Ouch, received wrong packet: $rx\n";
			}
			
			delete $expected{$rx};
			
			my $rx_l = length($rx) + 2;
			
			$outstanding -= $rx_l;
			$rxn++;
			$rxl += $rx_l;
		}
		
		#if ($txn % $max_speed == 0) {
		#	sleep(1);
		#}
	}
	
	if ($txq_l > 0) {
		warn "flushing the rest\n";
		$is_tx->sendline($txq, 1);
		$outstanding += $txq_l;
		$txq_l = 0;
		$txq = 0;
	}
	
	warn "reading the rest, have received $rxn packets, sent $txn\n";
	while ($outstanding > 0) {
		my $rx = $is_rx->getline(1);
		if (!defined $rx) {
			warn "rcved undefined\n";
			last;
		}
		if ($rx eq '') {
			warn "rcved empty\n";
			next;
		}
		#warn "rcved outstanding: $rx\n";
		next if ($rx =~ /^#/);
		if (!defined $expected{$rx}) {
			die "Ouch, received wrong packet: $rx\n";
		}
		
		delete $expected{$rx};
		
		my $rx_l = length($rx) + 2;
		$outstanding -= $rx_l;
		$rxn++;
		$rxl += $rx_l;
		#warn "now outstanding $outstanding\n";
	}
	
	warn "after reading the rest, have received $rxn packets, sent $txn, outstanding $outstanding bytes\n";
	$end_t = time();
	$dur_t = $end_t - $start_t;
	
	warn sprintf("took %.3f seconds, %.0f packets/sec\n", $dur_t, $rxn / $dur_t);
	
	if ($outstanding) {
		warn "missing: " . join("\n", sort keys %expected) . "\n";
	}
	
	ok($rxn, $txn, "Received wrong number of lines from blob");
	ok($rxl, $txl, "Received wrong number of bytes from blob");
	ok($outstanding, 0, "There are outstanding bytes in the server after timeout");
}

warn "Load testing full feed => UDP peer:\n";
load_test("F", $i_full, $udp);
warn "Load testing UDP peer => full feed:\n";
load_test("U", $udp, $i_full);


# disconnect ####################

$ret = $i_full->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_full->{'error'});
sleep(0.1); # let the server catch and log the disconnect

# stop

ok($p->stop(), 1, "Failed to stop product");

