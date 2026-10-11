
#
# Test IS2 login rejections: the server must answer a rejected login
# with a LOGIN_REPLY saying so, instead of leaving the client waiting.
#

use strict;
use is2skip;
use Test;
BEGIN { plan tests => 2 + 5 + 2 + 1 };
use runproduct;
use Ham::APRS::IS2;

my $p = new runproduct('is2-basic');
ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $server_call = "TESTING";

# usernames which must be rejected, and why
my @reject_login = (
	[ 'ABCDEFGHIJKLMNOP', 'too long' ],
	[ 'OH7/LZB', 'invalid characters' ],
	[ 'pass', 'built-in disallowed username' ],
	[ 'LOGINA', 'disallowed by configuration' ],
	[ $server_call, 'our server id' ],
);

foreach my $r (@reject_login) {
	my($call, $why) = @{ $r };

	my $is = new Ham::APRS::IS2("localhost:56152", $call);
	my $ret = $is->connect('retryuntil' => 8);

	# the client must get a login reply with a failure, not a timeout
	ok((!$ret && $is->{'error'} =~ /login failed/) ? 1 : 0, 1,
		"Login as '$call' ($why) was not rejected with a login reply: " . $is->{'error'});

	$is->disconnect();
}

# a valid login still works

my $is = new Ham::APRS::IS2("localhost:56152", "OH7LZB");
ok(defined $is, 1, "Failed to initialize Ham::APRS::IS2");

my $ret = $is->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $is->{'error'});

$is->disconnect();

# stop

ok($p->stop(), 1, "Failed to stop product");

