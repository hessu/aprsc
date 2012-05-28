#
# Uplink Q construct tests
#

use Test;

use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS_Fake;

my @packets;

BEGIN {
	for (my $i = 1; $i < 500; $i++) {
		my $data = "packet $i";
		my $elems = int($i/7);
		my $append = $i % 7;
		#warn "$data elems $elems append $append\n";
		my @l;
		for (my $d = 0; $d < $elems; $d++) {
			push @l, sprintf("SRV%03d", $d);
		}
		if ($append) {
			push @l, "D" x $append;
		} elsif (@l) {
			$l[$#l] .= '*';
		}
		#warn join(',', @l) . "\n";
		my $packet = "SRC>DST,qAI," . join(',', @l) . ":$data";
		$packet =~ s/,,/,/g;
		#warn "$packet\n";
		push @packets, $packet;
	}
	
	plan tests => 9 + ($#packets+1) + 3 + 2;
};

ok(1); # If we made it this far, we're ok.

my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:54153', 'CORE1');
ok(defined $iss1, 1, "Test failed to initialize listening server socket");
$iss1->bind_and_listen();

my $iss6 = new Ham::APRS::IS_Fake('[::1]:54153', 'CORE6');
ok(defined $iss6, 1, "Test failed to initialize listening server socket on IPv6");
$iss6->bind_and_listen();

my $p = new runproduct('uplinks');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N0CALL-1";
my $server_call = "TESTING";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $is1 = $iss1->accept();
ok(defined $is1, (1), "Failed to accept connection 1 from server");
$iss1->send_login_prompt($is1);
my $log1 = $is1->getline_noncomment(1);
$iss1->send_login_ok($is1);

my $is6 = $iss6->accept();
ok(defined $is6, (1), "Failed to accept connection ipv6 from server");
$iss6->send_login_prompt($is6);
my $log2 = $is6->getline_noncomment(1);
$iss6->send_login_ok($is6);

my $ret;
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

# (1):
#istest::txrx(\&ok, $is1, $i_rx,
#	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBA,BLAA:testing qAI (1)",
#	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBA,BLAA,7F000001,$server_call:testing qAI (1)");

# (1):
foreach my $packet (@packets) {
	my $expect = $packet;
	$expect =~ s/:/,00000000000000000000000000000001,$server_call:/;
	if (length($expect) > 509) {
		$is6->sendline($packet);
		ok(1);
	} else {
		istest::txrx(\&ok, $is6, $i_rx,
			$packet,
			$expect);
	}
}

my $read1;
$read1 = $is6->getline_noncomment(1);
ok($read1, undef, "Ouch, received data from read-only upstream connection ipv6");
$read1 = $is1->getline_noncomment(1);
ok($read1, undef, "Ouch, received data from read-only upstream connection 1");
$read1 = $i_rx->getline_noncomment(1);
ok($read1, undef, "Ouch, received unexpected data from full stream");

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

ok($p->stop(), 1, "Failed to stop product");

