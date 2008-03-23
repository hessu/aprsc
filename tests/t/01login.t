
# Simple test to login to the server.

use Test;
BEGIN { plan tests => 7 };
use runproduct;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $is = new Ham::APRS::IS("localhost:10152", "N0CALL");
ok(defined $is, 1, "Failed to initialize Ham::APRS::IS");

my $ret = $is->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $is->{'error'});

$ret = $is->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $is->{'error'});

ok($p->stop(), 1, "Failed to stop product");

