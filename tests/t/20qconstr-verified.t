#
# First batch of Q construct tests:
# Feed packets from a verified client
#

use Test;
BEGIN { plan tests => 13 };
use runproduct;
use istest;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");


my $tx_call = "N0CALL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:10152", $tx_call);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:10152", "N0CALL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

# connect

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

#
# All packets
# {
#    Place into TNC-2 format
#    If a q construct is last in the path (no call following the qPT)
#       delete the qPT
# }
#  ... and will continue to add qAS
#

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,qAR:a4ufy",
	"SRC>DST,DIGI1,DIGI5*,qAS,$tx_call:a4ufy");

#
#    If the packet entered the server from a verified client-only connection AND the FROMCALL does not match the login:
#    {
#        if a q construct exists in the packet
#            if the q construct is at the end of the path AND it equals ,qAR,login
#                Replace qAR with qAo
#        else if the path is terminated with ,I
#        {
#            if the path is terminated with ,login,I
#                Replace ,login,I with qAo,login
#            else
#                Replace ,VIACALL,I with qAr,VIACALL
#        }
#        else
#            Append ,qAO,login
#        Skip to "All packets with q constructs"
#    }
#    

#
#    If a q construct exists in the header:
#        Skip to "All packets with q constructs"
#
#    If header is terminated with ,I:
#    {
#        If the VIACALL preceding the ,I matches the login:
#            Change from ,VIACALL,I to ,qAR,VIACALL
#        Else
#            Change from ,VIACALL,I to ,qAr,VIACALL
#    }

#
#    Else If the FROMCALL matches the login:
#    {
#        Append ,qAC,SERVERLOGIN
#        Quit q processing
#    }
#    Else
#        Append ,qAS,login
#    Skip to "All packets with q constructs"
#

istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST:aifyua",
	"$tx_call>DST,TCPIP*,qAC,$server_call:aifyua");

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*:test",
	"SRC>DST,DIGI1,DIGI2*,qAS,$tx_call:test");

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

