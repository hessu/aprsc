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
			$l[$#l] =~ s/(.)$/,Y/;
		}
		#warn join(',', @l) . "\n";
		my $packet = "SRC>DST,qAI," . join(',', @l) . ":$data";
		$packet =~ s/,,/,/g;
		#warn "$packet\n";
		push @packets, $packet;
	}
	
	plan tests => 7 + ($#packets+1) + 5;
};

ok(1); # If we made it this far, we're ok.

my $iss6 = new Ham::APRS::IS_Fake('[::1]:54153', 'FAKEIS');
ok(defined $iss6, 1, "Test failed to initialize listening server socket on IPv6");
$iss6->bind_and_listen();

my $p = new runproduct('uplinks');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $is6 = $iss6->accept();
ok(defined $is6, (1), "Failed to accept connection ipv6 from server");
ok($iss6->process_login($is6), 'ok', "Failed to accept login ipv6 from server");

my $ret;
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $maxlen = 509;
$maxlen = 510 if (defined $ENV{'TEST_PRODUCT'} && $ENV{'TEST_PRODUCT'} =~ /^javap/);

foreach my $packet (@packets) {
	my $expect = $packet;
	$expect =~ s/:/,FAKEIS,$server_call:/;
	#warn "tx: $packet\n";
	if (length($expect) > $maxlen) {
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
$read1 = $i_rx->getline_noncomment(1);
ok($read1, undef, "Ouch, received unexpected data from full stream");

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

ok($p->stop(), 1, "Failed to stop product");

