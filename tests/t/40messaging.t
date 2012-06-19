
#
# Test messaging features:
#
# On a filtered igate port (14580), no messages should come out at first.
# When a position of a station has been heard, messages for that station
# should come out.
# After such a message, the next following position transmitted within
# 30 minutes by the originator should be passed to the recipient's socket, too.
#
# Messages transmitted to any SSID must be passed?
#
# Are messages transmitted to objects passed, too? No.
#
# When a position has been heard, positions for the same callsign-ssid
# from other igates should come out too, to assist TX igates to know
# the station is on the Internet.
#

use Test;
BEGIN { plan tests => 8 + 9 + 1 + 2 + 2 + 6 + 4 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Encode;
use utf8;

my $enc_utf8 = find_encoding("UTF-8") || die "Could not load encoding UTF-8"; # note: strict UTF-8

ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $server_call = "TESTING";

my $login_tx = "N0GATE";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login_tx,
	'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

# We set a filter on the rx so that the helper packets get through
my $login_rx = "N1GATE";
my $i_rx = new Ham::APRS::IS("localhost:55580", $login_rx,
	'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $msg_src = "M1SRC";
my $msg_dst = "M1DST";
my($tx, $rx, $helper);

# first, verify that a message packet is not passed to a filtered port
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I::%-9.9s:message", $msg_dst);
$helper = "H1LP>APRS,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# verify that a position packet from the message originator is not passed
# to a filtered port
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:!6428.51N/02545.98E#");
$helper = "H1LP-5>APRS,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# now, transmit a position packet on the receiving filtered port
$tx = "$msg_dst-2>APRS,OH2RDG*,WIDE,$login_rx,I:!6028.51N/02505.68E# should pass";
$rx = "$msg_dst-2>APRS,OH2RDG*,WIDE,qAR,$login_rx:!6028.51N/02505.68E# should pass";
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx);

sleep(3);

# now, transmit a position packet on the receiving filtered port
$tx = "$msg_dst>APRS,OH2RDG*,WIDE,$login_rx,I:!6028.51N/02505.68E# should pass";
$rx = "$msg_dst>APRS,OH2RDG*,WIDE,qAR,$login_rx:!6028.51N/02505.68E# should pass";
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx);

# now, transmit a position packet on the receiving filtered port
$tx = "$msg_dst-3>APRS,OH2RDG*,WIDE,$login_rx,I:!6028.51N/02505.68E# should pass";
$rx = "$msg_dst-3>APRS,OH2RDG*,WIDE,qAR,$login_rx:!6028.51N/02505.68E# should pass";
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx);

# then, a message packet should magically pass!
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,%s,I::%-9.9s:message", $login_tx, $msg_dst);
$rx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,qAR,%s::%-9.9s:message", $login_tx, $msg_dst);
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# Another message! With UTF-8 content.
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,%s,I::%-9.9s:Blää  blåå 日本語{1d", $login_tx, $msg_dst);
$rx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,qAR,%s::%-9.9s:Blää  blåå 日本語{1d", $login_tx, $msg_dst);
$tx = $enc_utf8->encode($tx);
$rx = $enc_utf8->encode($rx);
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# Also, it should pass to another SSID!
# NO, javaprssrvr does not pass this.
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,%s,I::%-9.9s:message with SSID{a", $login_tx, $msg_dst . '-5');
#$rx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,qAR,%s::%-9.9s:message with SSID{a", $login_tx, $msg_dst . '-5');
#istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);
$helper = "H1LP>APRS,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass5";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# now, after a message has been transmitted, the complimentary position should pass
$tx = "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:!5528.51N/00505.68E# should pass compl";
$rx = "$msg_src>APRS,OH2RDG*,WIDE,qAR,$login_tx:!5528.51N/00505.68E# should pass compl";
istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);

# try a second complimentary position - it must be dropped.
$tx = "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:!5628.51N/00505.68E# should drop compl2";
$helper = "H1LP-C>APRS,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

# A position packet having TCPIP* in the path, coming from another connection,
# should be passed to the port where the station was heard without TCPIP*.
# "This is so the IGate can determine if there is a station it is hearing
# on RF is also directly connected to APRS-IS.  If the RF station is heard
# directly on APRS-IS, the IGate should NOT gate messages for that station
# to RF." (Pete Loveall, 17 May 2012, APRSSIG)

$rx = $tx = "$msg_src>APRS,TCPIP*,qAC,$msg_src:!5528.51N/00505.68E# should pass TCPIP*";
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx);

#
# Message to an OBJECT
#

my $msg_obj = 'OBJDST';
# transmit the object on the receiving filtered port
$tx = sprintf("$msg_dst>APRS,OH2RDG*,WIDE,$login_rx,I:;%-9.9s*111111z6028.51N/02505.68Ercomment", $msg_obj);
$rx = sprintf("$msg_dst>APRS,OH2RDG*,WIDE,qAR,$login_rx:;%-9.9s*111111z6028.51N/02505.68Ercomment", $msg_obj);
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx);

# no, it should not pass at the moment
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,%s,I::%-9.9s:message to object", $login_tx, $msg_obj);
#$rx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,qAR,%s::%-9.9s:message to object", $login_tx, $msg_obj);
#istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);
$helper = "H1LP>APRS,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass6";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

#
# Message to an ITEM
#

my $msg_item = 'ITEDST';
# transmit the item on the receiving filtered port
$tx = sprintf("$msg_dst>APRS,OH2RDG*,WIDE,$login_rx,I:)%s!6028.51N/02505.68Ercomment", $msg_item);
$rx = sprintf("$msg_dst>APRS,OH2RDG*,WIDE,qAR,$login_rx:)%s!6028.51N/02505.68Ercomment", $msg_item);
istest::txrx(\&ok, $i_rx, $i_tx, $tx, $rx);

# no, it should not pass at the moment
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,%s,I::%-9.9s:message to item", $login_tx, $msg_item);
#$rx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,qAR,%s::%-9.9s:message to item", $login_tx, $msg_item);
#istest::txrx(\&ok, $i_tx, $i_rx, $tx, $rx);
$helper = "H1LP>APRS,OH2RDG*,WIDE:!6028.51N/02505.68E# should pass7";
istest::should_drop(\&ok, $i_tx, $i_rx, $tx, $helper);

#
# Connect another igate and see what happens when there are
# two gates hearing the same station!
#

# We set a filter on the rx so that the helper packets get through
my $login_rx2 = "N2GATE";
my $i_rx2 = new Ham::APRS::IS("localhost:55580", $login_rx2,
	'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_rx2, 1, "Failed to initialize Ham::APRS::IS");

$ret = $i_rx2->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx2->{'error'});

# Now, transmit a position packet on the second receiving filtered port.
# It will come out on the first receiving filtered port due to the
# range filter *and* due to it being heard there, too.
$tx = "$msg_dst>APRS,OH2RDG*,WIDE,$login_rx,I:!6028.51N/02505.68E# should pass 2nd";
$rx = "$msg_dst>APRS,OH2RDG*,WIDE,qAr,$login_rx:!6028.51N/02505.68E# should pass 2nd";
istest::txrx(\&ok, $i_rx2, $i_tx, $tx, $rx);
my $read1 = $i_rx->getline_noncomment(1);
ok($read1, $rx, "Got wrong line from first rx port");

# then, a message packet should magically pass! To both!
$tx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,%s,I::%-9.9s:two gates", $login_tx, $msg_dst);
$rx = sprintf("$msg_src>APRS,OH2RDG*,WIDE,qAR,%s::%-9.9s:two gates", $login_tx, $msg_dst);
istest::txrx(\&ok, $i_tx, $i_rx2, $tx, $rx);
$read1 = $i_rx->getline_noncomment(1);
ok($read1, $rx, "Got wrong message line from first rx port");


# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_rx2->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx2->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

