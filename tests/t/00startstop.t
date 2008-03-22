
# Simple test to start and stop the product.

use Test;
BEGIN { plan tests => 4 };
use runproduct;
ok(1); # If we made it this far, we're ok.

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");
ok($p->stop(), 1, "Failed to stop product");

