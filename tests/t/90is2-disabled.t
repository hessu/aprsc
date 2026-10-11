
#
# Test that a server built without IS2 (the default, USE_IS2 is not defined
# in config.h) rejects every IS2 configuration keyword, so that IS2 can not
# be enabled by configuration. Skipped when IS2 is enabled.
#

use strict;
use is2skip 'disabled';
use Test;

my @is2_lines = (
	'Listen "IS2 full" is2fullfeed tcp 0.0.0.0 56152',
	'Listen "IS2 igate" is2igate tcp 0.0.0.0 56580',
	'Listen "IS2 dupes" is2dupefeed tcp 0.0.0.0 56153',
	'Uplink is2up full tcp 127.0.0.1 10154 is2',
	'PeerGroupKey 1 firstkey',
	'PeerGroupKeyTransmit 1',
);

BEGIN { plan tests => 1 + 6 };

mkdir('tmp');
my $base = 'cfg-aprsc/basic';

sub config_check($)
{
	my($extra) = @_;

	open(my $in, '<', $base) || die "could not read $base: $!";
	open(my $out, '>', 'tmp/is2-disabled.cfg') || die "could not write tmp/is2-disabled.cfg: $!";
	print $out $_ while (<$in>);
	print $out "\n$extra\n" if (defined $extra);
	close($in);
	close($out);

	# the config check reads files relative to the tests directory
	system('../src/aprsc -y -c tmp/is2-disabled.cfg >/dev/null 2>&1');
	return $? >> 8;
}

# the control: the plain configuration is accepted
ok(config_check(undef), 0, "The configuration without IS2 lines was rejected");

foreach my $line (@is2_lines) {
	ok(config_check($line) != 0 ? 1 : 0, 1, "Server accepted a configuration with IS2 enabled: $line");
}

unlink('tmp/is2-disabled.cfg');

