#
# Uplink Q construct tests
#

use Test;
BEGIN { plan tests => 2 + 5 + 2 + 2 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS_Fake;
ok(1); # If we made it this far, we're ok.

my $iss1 = new Ham::APRS::IS_Fake('127.0.0.1:54153', 'CORE1');
ok(defined $iss1, 1, "Test failed to initialize listening server socket (IPv4)");
$iss1->bind_and_listen();

my $p = new runproduct('uplinks');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N0CALL-1";
my $server_call = "TESTING";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

#warn "accepting\n";

my $is1 = $iss1->accept();
ok(defined $is1, (1), "Failed to accept connection 1 from server");

#warn "sending login prompt\n";
$iss1->send_login_prompt($is1);
#warn "sending login ok\n";
$iss1->send_login_ok($is1);

my $ret;
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

#
#    If trace is on, the q construct is qAI, or the FROMCALL is on the server's trace list:
#    {
#        If the packet is from a verified port where the login is not found after the q construct:
#            (1) Append ,login
#        else if the packet is from an outbound connection
#            (2) Append ,IPADDR
#
#        (3) Append ,SERVERLOGIN
#    }
#

#warn "doing test 1\n";

# (2):
istest::txrx(\&ok, $is1, $i_rx,
	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBA,BLAA:testing qAI (1)",
	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBA,BLAA,7F000001,$server_call:testing qAI (1)");

# unbind the IPv4 server and create IPv6 server
$iss1->unbind();

#warn "switching to ipv6\n";

my $iss6 = new Ham::APRS::IS_Fake('[::1]:54153', 'CORE6');
ok(defined $iss6, 1, "Test failed to initialize listening server socket on IPv6");
$iss6->bind_and_listen();

#warn "disconnecting uplink 1\n";

$is1->disconnect();

#warn "accepting ipv6 connect\n";

my $is6 = $iss6->accept();
ok(defined $is6, (1), "Failed to accept connection ipv6 from server");
$iss6->send_login_prompt($is6);
$iss6->send_login_ok($is6);

# (2), ipv6:
istest::txrx(\&ok, $is6, $i_rx,
	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR,BLAA:testing qAI (ipv6)",
	"SRC>DST,DIGI1,DIGI2*,qAI,FOOBAR,BLAA,00000000000000000000000000000001,$server_call:testing qAI (ipv6)");

# disconnect
$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

ok($p->stop(), 1, "Failed to stop product");

