#########################
# Test the client and server socket timeouts
#########################

use Test;

my $id_ok;

BEGIN {
	plan tests => 15
};

use Ham::APRS::IS_Fake;
use Ham::APRS::IS;
use runproduct;
use Time::HiRes qw(time sleep);

my $ret;

ok(1); # modules load fine

my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:57153', 'CORE1');
ok(1); # there's a working socket

$iss1->bind_and_listen();
ok(1);

# initialize the product runner using the basic configuration
my $p = new runproduct('aggregator');
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $is1 = $iss1->accept();
ok(defined $is1, (1), "Failed to accept connection 1 from server");
ok($iss1->process_login($is1), 'ok', "Failed to accept login 1 from server");

my $t_start = time();

$read1 = $is1->getline_noncomment(30);
ok($read1, undef, "Ouch, got data from server when none should have been available");

my $t_end = time();
my $t_dif = $t_end - $t_start;
warn sprintf("\ntimeout on uplink socket took %.3f s, should take 10s\n", $t_dif);
ok($t_dif > 9 && $t_dif < 15);

# create client connection
my $cl = new Ham::APRS::IS("localhost:56152", 'CL1ENT');
ok(defined $cl, 1, "Failed to initialize Ham::APRS::IS");
$ret = $cl->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $cl->{'error'});

$t_start = time();

$read1 = $cl->getline_noncomment(60);
ok($read1, undef, "Ouch, received data from server on client connection when none should have been available");

$t_end = time();
$t_dif = $t_end - $t_start;
warn sprintf("\ntimeout on client socket took %.3f s, should take 20s\n", $t_dif);
ok($t_dif > 19 && $t_dif < 25);

$ret = $is1->disconnect();
ok($ret, 1, "Failed to disconnect 1: " . $is1->{'error'});

ok($p->stop(), 1, "Failed to stop product");


#########################


