
#
# Test HTTP status service, only on aprsc
#

use Test;

BEGIN {
	plan tests => (!defined $ENV{'TEST_PRODUCT'} || $ENV{'TEST_PRODUCT'} =~ /aprsc/) ? 2 + 16 + 1 : 0;
};

if (defined $ENV{'TEST_PRODUCT'} && $ENV{'TEST_PRODUCT'} !~ /aprsc/) {
	exit(0);
}

use runproduct;
use LWP;
use LWP::UserAgent;
use HTTP::Request::Common;
use JSON::XS;

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

my($req, $res, $j);

# transfer-encodings that the client cant decode (compression)
my $can_accept = HTTP::Message::decodable;

$req = HTTP::Request::Common::GET("http://127.0.0.1:55501/");
$res = $ua->simple_request($req);
ok($res->code, 200, "HTTP GET of status server front page returned wrong response code, message: " . $res->message);

$req->header('Accept-Encoding', $can_accept);
$res = $ua->simple_request($req);
ok($res->code, 200, "HTTP GET (compressed) of status server front page returned wrong response code, message: " . $res->message);


$req = HTTP::Request::Common::GET("http://127.0.0.1:55501/status.json");
$res = $ua->simple_request($req);
ok($res->code, 200, "HTTP GET of status server status.json returned wrong response code, message: " . $res->message);
$j = $json->decode($res->decoded_content(charset => 'none'));
ok(defined $j, 1, "JSON decoding of status.json failed");
ok(defined $j->{'server'}, 1, "status.json does not define 'server'");

$req->header('Accept-Encoding', $can_accept);
$res = $ua->simple_request($req);
ok($res->code, 200, "HTTP GET (compressed) of status server status.json returned wrong response code, message: " . $res->message);
$j = $json->decode($res->decoded_content(charset => 'none'));
ok(defined $j, 1, "JSON decoding of (compressed) status.json failed");
ok(defined $j->{'server'}, 1, "status.json (compressed) does not define 'server'");


$req = HTTP::Request::Common::GET("http://127.0.0.1:55501/counterdata?totals.tcp_bytes_rx");
$res = $ua->simple_request($req);
ok($res->code, 200, "HTTP GET of status server /counterdata?totals.tcp_bytes_rx returned wrong response code, message: " . $res->message);
$j = $json->decode($res->decoded_content(charset => 'none'));
ok(defined $j, 1, "JSON decoding of /counterdata?totals.tcp_bytes_rx failed");
ok(defined $j->{'interval'}, 1, "counterdata json does not define 'interval'");
ok(defined $j->{'values'}, 1, "counterdata json does not define 'values'");

$req->header('Accept-Encoding', $can_accept);
$res = $ua->simple_request($req);
ok($res->code, 200, "HTTP GET (compressed) of status server /counterdata?totals.tcp_bytes_rx returned wrong response code, message: " . $res->message);
$j = $json->decode($res->decoded_content(charset => 'none'));
ok(defined $j, 1, "JSON decoding of (compressed) /counterdata?totals.tcp_bytes_rx failed");
ok(defined $j->{'interval'}, 1, "counterdata json (compressed) does not define 'interval'");
ok(defined $j->{'values'}, 1, "counterdata json (compressed) does not define 'values'");


# stop

ok($p->stop(), 1, "Failed to stop product");

