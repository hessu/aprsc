#
# Test filters for parsing of greatly repeated filters (single filter gets extended)
# TODO: run this threaded, with multiple copies, has a better chance of finding
# a bug.
#

my $buddyrounds = 300;

use Test;
BEGIN { plan tests => 2 + 3*300 + 1 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Time::HiRes qw(sleep);

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $server_call = "TESTING";
my $i_tx;

for (my $i = 0; $i < $buddyrounds; $i++) {
	my @filters;
	for (my $b = 0; $b < $i; $b++) {
		push @filters, 'b/BUDDYYYY' . $b . '-42';
	}
	$i_tx = new Ham::APRS::IS("localhost:55580", $login, 'filter' => join(' ', @filters), 'nopass' => 1);
	ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");
	$ret = $i_tx->connect('retryuntil' => 8);
	ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});
	$ret = $i_tx->disconnect();
	ok($ret, 1, "Failed to disconnect from the server: " . $i_tx->{'error'});
}


ok($p->stop(), 1, "Failed to stop product");
