#
# First batch of Q construct tests:
# Feed packets from a verified client - CLIENT-ONLY PORT
#

use Test;
BEGIN { plan tests => 41 };
use runproduct;
use istest;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('qconstr-protocols');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");


my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx = new Ham::APRS::IS("localhost:55581", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $i_rx = new Ham::APRS::IS("localhost:55152", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

# connect, initially to the client-only port 55581

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

# Not in the Q algorithm, but:
# Packets having srccall == login, and having no Q construct, must have
# their digipeater path truncated away and replaced with ,TCPIP* and
# then the Q construct.

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST:tcpip-path-replace1",
	"$login>DST,TCPIP*,qOC,TESTING:tcpip-path-replace1");

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST,DIGI1,DIGI5*:tcpip-path-replace2",
	"$login>DST,TCPIP*,qOC,TESTING:tcpip-path-replace2");

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST,TCPIP*:tcpip-path-replace3",
	"$login>DST,TCPIP*,qOC,TESTING:tcpip-path-replace3");

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST,qOR,$login:tcpip-path-replace4",
	"$login>DST,TCPIP*,qOC,TESTING:tcpip-path-replace4");

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST,WIDE2-2,qOR,$login:tcpip-path-replace5",
	"$login>DST,TCPIP*,qOC,TESTING:tcpip-path-replace5");

# Not in the algorithm, but done in javap/javap4/aprsc:
# Packets having other protocol than 'A' in 'qO', should be dropped.
istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,qAR,GATES:testing wrong Q protocol ID",
	"SRC>DST:dummy"); # will pass (helper packet)

#
# All packets
# {
#    Place into TNC-2 format
#    If a q construct is last in the path (no call following the qPT)
#       delete the qPT
# }
#  ... and will continue to add qAO
#
# This test intentionally has a qAR without a trailing call, and
# it'll be converted to a qAO:

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,qOR:a4ufy",
	"SRC>DST,DIGI1,DIGI5*,qOO,$login:a4ufy");

# It's not in the algorithm, but:
# if a path element after the q construct has a '*' or other crap
# in the callsign, the packet is dropped.

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qOR,GATES*:testing * after Q construct",
	"SRC>DST:dummy"); # will pass (helper packet)

#
#    If the packet entered the server from a verified client-only connection AND the FROMCALL does not match the login:
#    {
#        if a q construct exists in the packet
#            if the q construct is at the end of the path AND it equals ,qAR,login
#                (1) Replace qAR with qAo
#            (5) else: skip to "all packets with q constructs")
#        else if the path is terminated with ,I
#        {
#            if the path is terminated with ,login,I
#                (2) Replace ,login,I with qAo,login
#            else
#                (3) Replace ,VIACALL,I with qAr,VIACALL
#        }
#        else
#            (4) Append ,qAO,login
#        Skip to "All packets with q constructs"
#    }
#    

# (1)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qOR,$login:testing (1)",
	"SRCCALL>DST,DIGI1*,qOo,$login:testing (1)");
# (2)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,$login,I:testing (2)",
	"SRCCALL>DST,DIGI1*,qOo,$login:testing (2)");
# (3)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,IGATE,I:testing (3)",
	"SRCCALL>DST,DIGI1*,qOo,IGATE:testing (3)");
# (4)
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*:testing (4)",
	"SRCCALL>DST,DIGI1*,qOO,$login:testing (4)");

# (5) - any other (even unknown) q construct is passed intact
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qOF,$login:testing (5)",
	"SRCCALL>DST,DIGI1*,qOF,$login:testing (5)");

#
# reconnect to the non-client-only port
#

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$i_tx = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

# for loop testing, also make a second connection
my $login_second = "MYC4LL-5";
$i_tx2 = new Ham::APRS::IS("localhost:55152", $login_second);
ok(defined $i_tx2, 1, "Failed to initialize Ham::APRS::IS");
$ret = $i_tx2->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect twice to the server: " . $i_tx->{'error'});

#
#    If a q construct exists in the header:
#        (a1) Skip to "All packets with q constructs"
#
# Hmm, javaprssrvr doesn't seem to implement this, goes to the qAC path

#istest::txrx(\&ok, $i_tx, $i_rx,
#	"$login>DST,DIGI1*,qAR,$login:testing (a1)",
#	"$login>DST,DIGI1*,qAR,$login:testing (a1)");


#    If header is terminated with ,I:
#    {
#        If the VIACALL preceding the ,I matches the login:
#            (b1) Change from ,VIACALL,I to ,qAR,VIACALL
#        Else
#            (b2) Change from ,VIACALL,I to ,qAr,VIACALL
#    }

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,$login,I:Asdf (b1)",
	"SRC>DST,DIGI1,DIGI5*,qOR,$login:Asdf (b1)");

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI5*,N5CAL,I:Asdf (b2)",
	"SRC>DST,DIGI1,DIGI5*,qOr,N5CAL:Asdf (b2)");

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
# Note: Only one TCPIP* should be inserted.

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST:aifyua",
	"$login>DST,TCPIP*,qOC,$server_call:aifyua");

istest::txrx(\&ok, $i_tx, $i_rx,
	"$login>DST,TCPIP*:gaaee",
	"$login>DST,TCPIP*,qOC,$server_call:gaaee");

istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*:test",
	"SRC>DST,DIGI1,DIGI2*,qOS,$login:test");


#
# All packets with q constructs:
# {
#     if ,qAZ, is the q construct:
#     {
#         Dump to the packet to the reject log
#         Quit processing the packet
#     }
#

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qOZ,$login:testing (qOZ)", # should drop
	"SRC>DST:dummy"); # will pass (helper packet)

#
#     If ,SERVERLOGIN is found after the q construct:
#     {
#         Dump to the loop log with the sender's IP address for identification
#         Quit processing the packet
#     }

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qOR,$server_call:testing (,SERVERLOGIN)", # should drop
	"SRC>DST:dummy"); # will pass (helper packet)

#
#    If a callsign-SSID is found twice in the q construct:
#    {
#        Dump to the loop log with the sender's IP address for identification
#        Quit processing the packet
#    }
#

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI1*,qOI,FOOBAR,ASDF,ASDF,BARFOO:testing (dup call)", # should drop
	"SRC>DST:dummy"); # will pass (helper packet)

#
#    If a verified login other than this login is found in the q construct
#    and that login is not allowed to have multiple verified connects (the
#    IPADDR of an outbound connection is considered a verified login):
#    {
#        Dump to the loop log with the sender's IP address for identification
#        Quit processing the packet
#    }
#
# (to test this, we made a second connection using call $login_second)

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI*,qOI,$login_second,$login:testing (verified call loop)", # should drop
	"SRC>DST:dummy"); # will pass (helper packet)

#
#    If the packet is from an inbound port and the login is found after the q construct but is not the LAST VIACALL:
#    {
#        Dump to the loop log with the sender's IP address for identification
#        Quit processing the packet
#    }
#

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRCCALL>DST,DIGI*,qOI,$login,M0RE:testing (login not last viacall)", # should drop
	"SRC>DST:dummy"); # will pass (helper packet)

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

# (1):
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*,qOI,FOOBAR:testing qOI (1)",
	"SRC>DST,DIGI1,DIGI2*,qOI,FOOBAR,$login,$server_call:testing qOI (1)");

# (2) needs to be tested elsewhere
# (3):
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1,DIGI2*,qOI,$login:testing qOI (3)",
	"SRC>DST,DIGI1,DIGI2*,qOI,$login,$server_call:testing qOI (3)");


#
# qAS appending bug, in javaprssrvr 3.15:
# packet coming from a broken DPRS gateway with no dstcall gets a new
# qAS,$login appended at every javaprssrvr on the way, and becomes
# qAS,FOO,qAS,BAR,qAS,ASDF...
#

istest::txrx(\&ok, $i_tx, $i_rx,
	"K1FRA>qOR,K1RFI-C,qOS,$login:/281402z4144.72N/07125.65W>178/001",
	"K1FRA>qOR,K1RFI-C,qOS,$login:/281402z4144.72N/07125.65W>178/001");

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});
$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

