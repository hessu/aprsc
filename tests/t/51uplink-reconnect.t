#
# Test uplink reconnection after flooding the upstream obuf
#

use Test;

my $rounds = 3;

BEGIN { plan tests => 8 + 3*4 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS_Fake;
ok(1); # If we made it this far, we're ok.

my $upstream_call = 'FAKEUP';

my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:10153', $upstream_call);
ok(defined $iss1, 1, "Test failed to initialize listening server socket (IPv4)");
$iss1->bind_and_listen();

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

#warn "accepting\n";

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

my $prefix = 'FL';
for (my $round = 0; $round < $rounds; $round += 1) {
	#warn "round $round\n";
	
	my $is1 = $iss1->accept();
	ok(defined $is1, (1), "Round $round: Failed to accept connection 1 from server");
	ok($iss1->process_login($is1), 'ok', "Round $round: Failed to accept login 1 from server");

	my $obuf = '';
	for (my $txn = 0; $txn < 10000; $txn++) {
		$obuf .= $prefix . ($txn % 10000 + 10) . ">APRS,qAR,OH9XYZ-5:!6028.51N/02505.68E# round $round packet $txn blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa END\r\n";
		my $len = length($obuf);
		if ($len > 32768) {
			#warn "writing $len\n";
			$i_tx->sendline($obuf, 1);
			$obuf = '';
		}
	}
	if ($obuf) {
		$i_tx->sendline($obuf, 1);
		$obuf = '';
	}
	
	my $is2 = $iss1->accept();
	ok(defined $is2, (1), "Round $round: Failed to accept reconnection from server");
	ok($iss1->process_login($is2), 'ok', "Round $round: Failed to accept reconnection login from server");
	
	$is1->disconnect();
	$is1 = $is1;
}

# disconnect
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

ok($p->stop(), 1, "Failed to stop product");

