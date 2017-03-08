
#
# Test live upgrade, only on aprsc
#

use Test;

BEGIN {
	plan tests => (!defined $ENV{'TEST_PRODUCT'} || $ENV{'TEST_PRODUCT'} =~ /aprsc/) ? 2 + 8 + 2 + 8 + 1 : 0;
};

if (defined $ENV{'TEST_PRODUCT'} && $ENV{'TEST_PRODUCT'} !~ /aprsc/) {
	exit(0);
}

use runproduct;
use LWP;
use LWP::UserAgent;
use HTTP::Request::Common;
use JSON::XS;
use Ham::APRS::IS;
use Time::HiRes qw( sleep time );
use istest;

# set up the JSON module
my $json = new JSON::XS;
   
if (!$json) {
	die "JSON loading failed";
}

$json->latin1(0);
$json->ascii(1);
$json->utf8(0);

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

# set up http client ############

my $ua = LWP::UserAgent->new;

$ua->agent(
	agent => "httpaprstester/1.0",
	timeout => 10,
	max_redirect => 0,
);

# test ###########################

my($req, $res);

# first get the status page without any traffic
$res = $ua->simple_request(HTTP::Request::Common::GET("http://127.0.0.1:55501/status.json"));
ok($res->code, 200, "pre-upgrade HTTP GET of status.json returned wrong response code, message: " . $res->message);
my $j1 = $json->decode($res->decoded_content(charset => 'none'));
ok(defined $j1, 1, "pre-upgrade JSON decoding ofstatus.json failed");
ok(defined $j1->{'server'}, 1, "pre-upgrade status.json does not define 'server'");

# connect an IS client and produce 1 traffic packet with 1 duplicate
my $login = "N5CAL-10";
my $i_tx = new Ham::APRS::IS("localhost:55580", $login);
ok(defined $i_tx, 1, "Failed to initialize Ham::APRS::IS");
my $ret = $i_tx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_tx->{'error'});

my $i_rx = new Ham::APRS::IS("localhost:55152", "N5CAL-2");
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");
$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# send a packet, a duplicate, and a third dummy packet
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,qAR,$login:foo1",
	"SRC>DST,qAR,$login:foo1");

# 11: send the same packet with a different digi path and see that it is dropped
istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1*,qAR,$login:foo1", # should drop
	"SRC>DST:dummy1", 1); # will pass (helper packet)

# delete old liveupgrade status file, ignore errors if it doesn't happen to exist yet
my $liveupgrade_json_old = "data/liveupgrade.json.old";
unlink($liveupgrade_json_old);

ok($p->signal('USR2'), 1, "Failed to signal product to live upgrade");

# it takes some time for aprsc to shut down and reload, wait for
# the new instance to finish reloading status.json, some 0.5s usually
# but a virtual machine in TA might be slow
my $maxwait = 10;
my $wait_start = time();
my $wait_end = time() + $maxwait;
while (time() < $wait_end && ! -e $liveupgrade_json_old) {
	sleep(0.1);
}
#warn sprintf("waited %.3f s\n", time() - $wait_start);
ok(-e $liveupgrade_json_old, 1, "live upgrade not done, timed out in $maxwait s, $liveupgrade_json_old not present");

# do the same test again - dupecheck cache has been lost in the
# upgrade
istest::txrx(\&ok, $i_tx, $i_rx,
	"SRC>DST,qAR,$login:foo1",
	"SRC>DST,qAR,$login:foo1");

istest::should_drop(\&ok, $i_tx, $i_rx,
	"SRC>DST,DIGI1*,qAR,$login:foo1", # should drop
	"SRC>DST:dumm21", 1); # will pass (helper packet)

# it takes some time for worker threads to accumulate statistics
sleep(1.5);

# get status counters
$res = $ua->simple_request(HTTP::Request::Common::GET("http://127.0.0.1:55501/status.json"));
ok($res->code, 200, "post-upgrade HTTP GET of status.json returned wrong response code, message: " . $res->message);

# validate that the counters include packets sent after the reload only (2 uniques, 1 dupe)
my $j2 = $json->decode($res->decoded_content(charset => 'none'));
ok(defined $j2, 1, "post-upgrade JSON decoding of status.json failed");
ok(defined $j2->{'dupecheck'}, 1, "post-upgrade status.json does not define 'dupecheck'");
ok($j2->{'dupecheck'}->{'uniques_out'}, 2, "post-upgrade uniques_out check");
ok($j2->{'dupecheck'}->{'dupes_dropped'}, 1, "post-upgrade dupes_dropped check");

# stop

ok($p->stop(), 1, "Failed to stop product");

