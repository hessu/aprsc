#########################
# Feed data to an aggregator server from two servers
# and validate that no dupes come out
#########################

use Test;

my @packets;

my $id_ok;

BEGIN {
	my $test_srcs = $ENV{'TEST_SRCCALLS'};
	my @test_srcl = split(',', $test_srcs);
	my $test_igate = $ENV{'TEST_IGATE'};
	my $id = $ENV{'TEST_ID'};
	my $tlm_seq = int(rand(1000));
	
	foreach my $src (@test_srcl) {
		push @packets, "$src>APZMDR,TRACE2-2,qAR,$test_igate:!/0(KkThq\">{2Oaprs.fi system testing COM_$id";
		push @packets, "$src>APZMDR,TRACE2-2,qAR,$test_igate:>system testing STA_$id status";
		push @packets, "$src>ID,qAR,$test_igate:Stupid testing BEACON_$id beacon";
		push @packets, "$src>APRS,WIDE2-2,qAR,${test_igate}::OH7LZB-10:MSG_$id";
		push @packets, "$src>APRS,WIDE2-2,qAR,${test_igate}:T#$tlm_seq,1,010,100,500,900,10101010";
		push @packets, "$src>APRS,WIDE2-2,qAR,${test_igate}:_03041723c344s009g014t038r000p000P000h38b10207wDVP";
		# bad packet to force raw packets flush
		push @packets, "$src>APZMDR,TRACE2-2,qAR,$test_igate:!/0(KBAD LAST BAD_$id";
	}
	
	plan tests => 9 + 2 + ($#packets+1)*2 + 2 + 2 + 2 + 1
};

use Ham::APRS::IS_Fake;
use runproduct;

my $ret;

ok(1); # modules load fine

my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:54153', 'CORE1');
my $iss2 = new Ham::APRS::IS_Fake('127.0.0.1:54154', 'CORE2');
ok(1); # there's a working socket

$iss1->bind_and_listen();
$iss2->bind_and_listen();
ok(1);

# initialize the product runner using the basic configuration
my $p = new runproduct('aggregator');
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $is1 = $iss1->accept();
ok(defined $is1, (1), "Failed to accept connection 1 from server");
my $is2 = $iss2->accept();
ok(defined $is2, (1), "Failed to accept connection 2 from server");

$iss1->send_login_prompt($is1);
$iss1->send_login_ok($is1);
$iss2->send_login_prompt($is2);
$iss2->send_login_ok($is2);

my $read1 = $is1->getline_noncomment(1);
my $read2 = $is2->getline_noncomment(2);
ok($read1, qr/^user TESTING pass 31421 /, "Did not receive 'user' login command on connection 1");
ok($read2, qr/^user TESTING pass 31421 /, "Did not receive 'user' login command on connection 2");

# create client connection
my $cl = new Ham::APRS::IS("localhost:55152", 'CL1ENT');
ok(defined $cl, 1, "Failed to initialize Ham::APRS::IS");
$ret = $cl->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $cl->{'error'});

foreach $i (@packets) {
	ok($is1->sendline($i), 1, "failed to write packet to socket 1");
	ok($is2->sendline($i), 1, "failed to write packet to socket 2");
}

sleep(3);

$read1 = $is1->getline_noncomment(1);
$read2 = $is2->getline_noncomment(2);
ok($read1, undef, "Ouch, received data from read-only upstream connection 1");
ok($read2, undef, "Ouch, received data from read-only upstream connection 2");

# read the packets from the client connection, validate that we get only
# a single copy of each packet
my %h;
my %g;
my $dupes_found = 0;
my $unknowns_found = 0;
foreach $i (@packets) { $h{$i} = 1; }
while (my $l = $cl->getline_noncomment(1)) {
	if (!defined $h{$l}) {
		$unknowns_found += 1;
	} elsif (defined $g{$l}) {
		$dupes_found += 1;
	} else {
		#warn "got line: $l\n";
		$g{$l} = 1;
	}
}

ok($dupes_found, 0, "found duplicate packets in output stream");
ok($unknowns_found, 0, "found unknown packets in output stream");

#my $read1 = istest::read_and_disconnect($sock1);
#my $read2 = istest::read_and_disconnect($sock2);

$ret = $is1->disconnect();
ok($ret, 1, "Failed to disconnect 1: " . $is1->{'error'});
$ret = $is2->disconnect();
ok($ret, 1, "Failed to disconnect 2: " . $is2->{'error'});

ok($p->stop(), 1, "Failed to stop product");


#########################


