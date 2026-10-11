
#
# Test that the UDP peer signing keys follow a configuration reload:
# a key removed from the configuration must no longer be accepted.
#

use strict;
use Test;
BEGIN { plan tests => 7 + 2 + 5 + 2 };
use runproduct;
use istest;
use Ham::APRS::IS;
use Ham::APRS::IS2;

# The server reloads its configuration file in place, so run it with
# a copy of the IS2 configuration which the test can modify.
my $cfg_src = 'cfg-aprsc/is2-basic';
my $cfg_tmp = 'tmp/is2-key-reload';
my $cfg_name = '../tmp/is2-key-reload'; # relative to cfg-aprsc/

sub write_config($)
{
	my($without_key) = @_;

	open(my $in, '<', $cfg_src) || die "could not read $cfg_src: $!";
	open(my $out, '>', $cfg_tmp) || die "could not write $cfg_tmp: $!";
	while (my $l = <$in>) {
		next if (defined $without_key && $l =~ /^PeerGroupKey\s+$without_key\s/);
		print $out $l;
	}
	close($in);
	close($out);
}

mkdir('tmp');
write_config(undef);

my $p = new runproduct($cfg_name);
ok(defined $p, 1, "Failed to initialize product runner");

# UDP peer socket, signing with key 2
my $is2_hmac_keys = {
    1 => "firstkey",
    2 => "secondkey",
    3 => "thirdkey",
};
my $udp = new Ham::APRS::IS2('127.0.0.1:16405', 'TESTIN-2', 'udp-peer' => 1, 'hmac-keys' => $is2_hmac_keys, 'hmac-key-tx' => 2);
ok(defined $udp, 1, "Failed to initialize Ham::APRS::IS2 UDP server socket");
ok($udp->bind_and_listen(), 1, "Failed to bind UDP server socket");
$udp->set_destination('127.0.0.1:16404', 'TESTING');

ok($p->start(), 1, "Failed to start product");

my $client = new Ham::APRS::IS("localhost:55152", "N5CAL-1");
ok(defined $client, 1, "Failed to initialize Ham::APRS::IS");

my $ret = $client->connect('retryuntil' => 8);
ok($ret, 1, "Failed to connect to the server: " . $client->{'error'});

ok($udp->is2_corepeer_negotiate(), 1, "Failed to negotiate IS2 corepeer link: " . $udp->{'error'});

# frames signed with key 2 pass before the reload
my $s = "SRC>DST,qAR,IGATE:signed with key 2 before reload";
istest::txrx(\&ok, $udp, $client, $s, $s);

# remove key 2 from the configuration and reload
write_config(2);
ok($p->signal('USR1'), 1, "Failed to signal product to reload configuration");
sleep(3);

# the peers are set up again on reload, so negotiate again
ok($udp->is2_corepeer_negotiate(), 1, "Failed to negotiate IS2 corepeer link after reload: " . $udp->{'error'});

# a frame signed with the removed key 2 must be dropped, key 1 still works
my $dropped = "SRC>DST,qAR,IGATE:signed with key 2 after reload";
ok($udp->send_packet($dropped), 1, "Failed to send packet signed with the removed key");

$udp->{'hmac-key-tx'} = 1;
$s = "SRC>DST,qAR,IGATE:signed with key 1 after reload";
ok($udp->send_packet($s), 1, "Failed to send packet signed with key 1");

my $r = $client->get_packet();
ok($r, $s, "Server forwarded a frame signed with a key removed from the configuration, or dropped a valid one");

# the server signs its own frames with key 1, which the peer still has
$s = "SRC>DST,qAR,IGATE:from client after reload";
istest::txrx(\&ok, $client, $udp, $s, $s);

# disconnect ####################

$ret = $client->disconnect();
ok($ret, 1, "Failed to disconnect from the server: " . $client->{'error'});

# stop

ok($p->stop(), 1, "Failed to stop product");

unlink($cfg_tmp);

