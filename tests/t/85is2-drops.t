
#
# Test dropping of IS2 packets whose packet data would not be valid
# on a plaintext APRS-IS link.
#

use strict;
use Test;
BEGIN { plan tests => 6 + 3 + 3 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login_tx = "N0GATE";
my $i_tx = new Ham::APRS::IS2("localhost:56580", $login_tx);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS2");

# the receiver is a plaintext full feed client, where an injected
# second line would show up as a separate packet
my $login_rx = "N1GATE";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login_rx);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

my $ret;
$ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# do the actual tests

my $msg_src = "M1SRC";
my $helper = "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:!6028.51N/02505.68E# helper";
my $inject = "INJECT>APRS,TCPIP*,qAC,T2FAKE:!6028.51N/02505.68E# injected";

# A CR or LF in the packet data would split the packet in two lines on a
# plaintext link, and the second line would bypass all validation.
istest::should_drop(\&ok, $i_tx, $i_rx, "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:>first\r\n$inject", $helper);
istest::should_drop(\&ok, $i_tx, $i_rx, "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:>first\n$inject", $helper);
istest::should_drop(\&ok, $i_tx, $i_rx, "$msg_src>APRS,OH2RDG*,WIDE,$login_tx,I:>first\r$inject", $helper);

# disconnect

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

