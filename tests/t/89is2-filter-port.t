
#
# Test that IS2 clients can only set filters on ports which allow user
# filters: a full feed port must refuse the PARAMETER_SET and keep
# sending everything, while the igate port applies it.
#

use strict;
use Test;
BEGIN { plan tests => 9 + 2 + 4 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login_tx = "N0GATE";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login_tx);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");

my $ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

# an IS2 client on the full feed port: the login works, but the filter
# request after it must be refused, and the connection must stay up
my $i_full = new Ham::APRS::IS2("localhost:56152", "N1FULL", 'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_full, 1, "Failed to initialize Ham::APRS::IS2");

$ret = $i_full->connect('retryuntil' => 8);
ok($ret, 0, "Full feed port accepted a filter from an IS2 client: " . $i_full->{'error'});
ok($i_full->{'state'}, 'connected', "Connection was lost after a refused filter: " . $i_full->{'error'});

# an IS2 client on the igate port, where filters are allowed
my $i_igate = new Ham::APRS::IS2("localhost:56580", "N1GATE", 'filter' => 'r/60.4752/25.0947/1');
ok(defined $i_igate, 1, "Failed to initialize Ham::APRS::IS2");

$ret = $i_igate->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the igate port with a filter: " . $i_igate->{'error'});

# a packet far outside the filter range goes to the full feed client,
# and the igate client gets only the one inside its range
my $outside = "M1SRC>APRS,OH2RDG*,WIDE,$login_tx,I:!4000.00N/01000.00E# outside the filter";
my $outside_rx = "M1SRC>APRS,OH2RDG*,WIDE,qAR,$login_tx:!4000.00N/01000.00E# outside the filter";
my $inside = "M1SRC>APRS,OH2RDG*,WIDE,$login_tx,I:!6028.51N/02505.68E# inside the filter";

istest::txrx(\&ok, $i_tx, $i_full, $outside, $outside_rx);
istest::should_drop(\&ok, $i_tx, $i_igate, $outside, $inside, 1, 1);

# disconnect

$ret = $i_igate->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_igate->{'error'});

$ret = $i_full->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_full->{'error'});

$ret = $i_tx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

