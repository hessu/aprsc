
#
# Test HTTP services
#

use Test;
BEGIN { plan tests => 4 + 2 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS;
use LWP;
use LWP::UserAgent;
use HTTP::Request::Common;

my $p = new runproduct('basic');

ok(defined $p, 1, "Failed to initialize product runner");
ok($p->start(), 1, "Failed to start product");

my $login = "N5CAL-1";
my $i_rx = new Ham::APRS::IS("localhost:55152", $login);
ok(defined $i_rx, 1, "Failed to initialize Ham::APRS::IS");

$ret = $i_rx->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $i_rx->{'error'});

# set up http client ############

my $ua = LWP::UserAgent->new;

$ua->agent(
	agent => "httpaprstester/1.0",
	timeout => 10,
	max_redirect => 0,
);

# test ###########################

my $data = "TEST>HTAPRS,TCPIP*:>http packet content";
my $out = "TEST>HTAPRS,TCPIP*,qAU,TESTING:>http packet content";
my $post = "user TEST pass 29939 vers httpaprstester 1.0\r\n"
	. "$data\r\n";
my $url = "http://127.0.0.1:55080/";
#$url = "http://he.fi/";
my $req = HTTP::Request::Common::POST($url);
#$req->header('Accept', 'text/plain');
$req->header('Accept-Type', 'text/plain');
$req->header('Content-Type', 'application/octet-stream');
$req->header('Content-Length', length($post));
$req->content($post);

my $res = $ua->simple_request($req);

ok($res->code, 200, "HTTP POST packet upload returned wrong response code, message: " . $res->message);

#istest::txrx(\&ok, $i_tx, $i_rx,
#	"SRC>DST,qAR,$login:$data",
#	"SRC>DST,qAR,$login:$data");

my $l = $i_rx->getline_noncomment();
ok($l, $out, "Got wrong line through HTTP uploading");

# disconnect ####################

$ret = $i_rx->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $i_rx->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

