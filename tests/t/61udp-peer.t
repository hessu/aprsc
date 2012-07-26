
#
# Test UDP core peers. While at it, check that the
# basic loop prevention rules work.
#
# 1) Traffic from upstreams goes to clients.
# 2) Traffic from core peers goes to clients.
# 3) Traffic does not pass between peers and upstreams.
# 4) Traffic from clients goes to core peers and upstreams.
#
# The testing order is selected so that the last packet proves
# the previous ones were not transitted to the wrong sockets.
#

use Test;
BEGIN { plan tests => 9 + 4 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS_Fake;
use Ham::APRS::IS_Fake_UDP;
use Time::HiRes qw(sleep);

my $p = new runproduct('basic');

# UDP peer socket
my $udp = new Ham::APRS::IS_Fake_UDP('127.0.0.1:16405', 'N0UDP');
ok(defined $udp, (1), "Failed to set up UDP server socket");
ok($udp->bind_and_listen(), 1, "Failed to bind UDP server socket");
$udp->set_destination('127.0.0.1:16404');

# TCP server socket
my $upstream_call = 'FAKEUP';
my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:10153', $upstream_call);
ok(defined $iss1, 1, "Test failed to initialize listening server socket (IPv4)");
$iss1->bind_and_listen();

# Start software
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# Set up client and connect
my $login = "N5CAL-1";
my $server_call = "TESTING";
my $client = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $client, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $client->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $client->{'error'});

# Accept connection from server
my $is1 = $iss1->accept();
ok(defined $is1, (1), "Failed to accept connection 1 from server");
ok($iss1->process_login($is1), 'ok', "Failed to accept login 1 from server");

# test ###########################

my $s;

# 1) from upstream to client
$s = "SRC>DST,qAR,IGATE:upstream to client";
istest::txrx(\&ok, $is1, $client, $s, $s);

# 2) from core peer to client
$s = "SRC>DST,qAR,IGATE:core peer to client";
istest::txrx(\&ok, $udp, $client, $s, $s);

# 4) from client to peers and upstreams
$s = "SRC>DST,qAR,IGATE:from client";
istest::txrx(\&ok, $client, $is1, $s, $s);

my $r = $udp->getline();
ok($r, $s, "Failed to pass packet from client to UDP peer");

# disconnect ####################

$ret = $client->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $client->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

