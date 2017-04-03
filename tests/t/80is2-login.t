
# Simple test to login to the server.

use Test;
BEGIN { plan tests => 7 };
use runproduct;
use Ham::APRS::IS2;
ok(1); # If we made it this far, we're ok.

# initialize the product runner using the basic configuration
my $p = new runproduct('is2-basic');

# start the software up
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# connect to the server
my $is = new Ham::APRS::IS2("localhost:56152", "OH7LZB");
ok(defined $is, 1, "Failed to initialize Ham::APRS::IS2");

my $ret = $is->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $is->{'error'});

$ret = $is->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $is->{'error'});

ok($p->stop(), 1, "Failed to stop product");

