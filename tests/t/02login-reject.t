
# Test login rejections

use Test;
BEGIN { plan tests => 5 };
use runproduct;
use Ham::APRS::IS;
ok(1); # If we made it this far, we're ok.

# initialize the product runner using the basic configuration
my $p = new runproduct('basic');

# start the software up
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# Callsigns which should be rejected on login
my @reject_login = (
	# static, built-in list
	"pass",
	# dynamic configurable rejections, with wildcards
	"LOGINA",
	"loginb",
	"0prrej",
	"mi0rej",
	"sufrej"
);

my $fail = 0;

# try to connect with each of the callsigns
foreach my $s (@reject_login) {
	# connect to the server
	my $is = new Ham::APRS::IS("localhost:55152", $s);
	if (!$is) {
		warn "Failed to initialize Ham::APRS::IS: $s\n";
		$fail++;
	}
	
	my $ret = $is->connect('retryuntil' => 8);
	if ($ret) {
		warn "Succeeded to connect to the server as '$s': " . $is->{'error'};
		$fail++;
	} else {
		#warn "OK, rejected: " . $is->{'error'} . "\n";
	}
	
	$is->disconnect();
}

ok($fail, 0, "Server accepted logins which it should have rejected.");

ok($p->stop(), 1, "Failed to stop product");

