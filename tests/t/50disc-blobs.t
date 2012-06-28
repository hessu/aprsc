
#
# Test disconnection in the middle of transmitting a packet
#

use Test;
BEGIN { plan tests => 12 };
use runproduct;
use istest;
use Ham::APRS::IS;

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login_tx = "N0GAT";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login_tx);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $login_rx = "N1GAT";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login_rx);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# Send a packet halfway and disconnect
my $tx = "M0SRC>APRS,OH2RDG*,WIDE,qAR,$login_tx:!6028.51N/02505.68E#this packet trunca";
$i_tx->sendline($tx, 1);
sleep(1);
$i_tx->disconnect();

my $r = $i_rx->getline_noncomment(2);
ok($r, undef, "Got message without CRLF truncated due to disconnect");

# connect again

$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

#
############################################
# Do a blob transmit test
#

my $blobsize = 32*1024;
$tx = '';
my $txl = 0;
my $txn = 0;
my @l = ();

while ($txl < $blobsize) {
	$s = "M" . ($txn + 10) . ">APRS,qAR,$login_tx:!6028.51N/02505.68E#should pass blaa blaa blaa blaa blaa blaa blaa blaa blaa blaa END";
	push @l, $s;
	$tx .= $s . "\r\n";;
	$txl += length($s); 
	$txn++;
}

warn " blobsize $txl ($blobsize) with $txn packets\n";

my $sent = $i_tx->sendline($tx, 1);
ok($sent, 1, "Failed to transmit blob of $txl bytes");

my $rxl = 0;
my $rxn = 0;
while (my $rx = $i_rx->getline_noncomment(1)) {
	if ($rx ne $l[$rxn]) {
		warn "got wrong packet: $rx\n";
	}
	$rxn++;
	$rxl += length($rx);
}

ok($rxn, $txn, "Received wrong number of lines from blob");
ok($rxl, $txl, "Received wrong number of bytes from blob");


# stop

ok($p->stop(), 1, "Failed to stop product");

