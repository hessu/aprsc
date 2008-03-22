
# Simple test to start and stop the product.

use Test;
BEGIN { plan tests => 4 };
use runproduct;
ok(1); # If we made it this far, we're ok.

ok(runproduct::init(), 1, "Failed to initialize product runner");
ok(runproduct::start("basic"), 1, "Failed to start product");
ok(runproduct::stop(), 1, "Failed to stop product");

