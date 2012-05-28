#
# Second batch of Q construct tests
# Feed packets from an unverified client
#

use Test;
BEGIN { plan tests => 17 };
use runproduct;
use istest;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

$tx_call = "N0CALL-1";
$server_call = "TESTING";
$i_tx = new Ham::APRS::IS("localhost:55152", $tx_call, 'nopass' => 1);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

$i_rx = new Ham::APRS::IS("localhost:55152", "N0CALL-2", 'nopass' => 1);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

# connect

$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

#
# Currently, all packets from an unverified connection, with the FROMCALL
# not matching the login call, will be dropped. Send a few of those packets
# here: if they will be forwarded, the following tests will fail.
#

# basic packet with no Q construct
my $position = "!6028.52N/02505.61E# Testing";
istest::should_drop(\&ok, $i_tx, $i_rx,
	"OH7LZB>DST:$position", # should drop
	"$tx_call>DST,qAR,BLAH:$position"); # helper

# basic packet with Q construct inserted by normal igate
istest::should_drop(\&ok, $i_tx, $i_rx,
	"OH7LZB-1>DST,qAR,$tx_call:$position", # should drop
	"$tx_call>DST,qAR,BLAH:$position"); # helper

# basic packet with old-style ,I construct inserted by igate
istest::should_drop(\&ok, $i_tx, $i_rx,
	"OH7LZB-2>DST,$tx_call,I:$position", # should drop
	"$tx_call>DST,qAR,BLAH:$position"); # helper

#
#    If the packet entered the server from an unverified connection AND the FROMCALL
#    equals the client login AND the header has been successfully converted to TCPXX
#    format (per current validation algorithm):
#    {
#        (All packets not deemed "OK" from an unverified connection should be dropped.)
#        (1) if a q construct with a single call exists in the packet
#            Replace the q construct with ,qAX,SERVERLOGIN
#        (2) else if more than a single call exists after the q construct
#            Invalid header, drop packet as error
#        (3) else
#            Append ,qAX,SERVERLOGIN
#        Quit q processing
#    }
#
# Must send a proper position packet - unverified clients
# can only send position and WX packets.
#

$position = "!6028.51N/02505.68E# Testing";

# (1)
istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST,DIGI,qAR,BLAH:$position",
	"$tx_call>DST,TCPXX*,qAX,$server_call:$position");

# (2) should be dropped, so don't expect anything 
# if it's not dropped, the *next* test will fail
# NOTE: javaprssrvr seems to forward with ,qAX,SERVERLOGIN
#istest::should_drop(\&ok, $i_tx, $i_rx,
#	"$tx_call>DST,qAR,CALL1,CALL2:$position", # should drop
#	"$tx_call>DST,qAR,BLAH:$position"); # helper

# (3)
$position .= '.'; # make it unique
istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST,DIGI:$position",
	"$tx_call>DST,TCPXX*,qAX,$server_call:$position");

$position .= '.';
istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST,DIGI1,DIGI5*,N0CALL,I:$position",
	"$tx_call>DST,TCPXX*,qAX,$server_call:$position");


# Although qAZ says "drop" in the end of the Q algorithm, this will
# match case (1) in the case of an unverified connection and
# will be forwarded, with ,qAX,SERVERLOGIN
$position .= '.';
istest::txrx(\&ok, $i_tx, $i_rx,
	"$tx_call>DST,DIGI,qAZ,$tx_call:$position",
	"$tx_call>DST,TCPXX*,qAX,$server_call:$position");

# TODO: should verify the rest of the things in the end of the
# algorithm - everything should come out with a qAX,SERVERLOGIN

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

