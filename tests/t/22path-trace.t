#
# Test path tracing (qAI)
#

use Test;
BEGIN { plan tests => 7 + 2 + 3 };
use runproduct;
use istest;
use Ham::APRS::IS;

ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

$tx_call = "N0CALL-1";
$server_call = "TESTING";
$i_tx = new Ham::APRS::IS("localhost:55152", $tx_call);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

$i_rx = new Ham::APRS::IS("localhost:55152", "N0CALL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

# connect

$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

#
# Test that qAI packets.
#
#    If trace is on, the q construct is qAI, or the FROMCALL is on the server's trace list:
#    {
#        (1) If the packet is from a verified port where the login is not found after the q construct:
#            Append ,login
#        (2) else if the packet is from an outbound connection
#            Append ,IPADDR
#        
#        (3) Append ,SERVERLOGIN
#    }
#}
#

# (1)
my $position = "!6028.52N/02505.61E# Testing";
istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST,DIGI,qAI,BLAH:$position",
	"$tx_call>DST,DIGI,qAI,BLAH,$tx_call,$server_call:$position");

# (3)
$position .= '.'; # make it unique to pass dupe check
istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST,DIGI,qAI,$tx_call:$position",
	"$tx_call>DST,DIGI,qAI,$tx_call,$server_call:$position");

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

