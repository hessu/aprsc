package is2skip;

=head1 NAME

is2skip - skip a test file depending on whether the server is built with IS2

=head1 SYNOPSIS

  use is2skip;			# skip unless IS2 is enabled
  use is2skip 'disabled';	# skip if IS2 is enabled

Use it first in the test file, before the test plan.

IS2 is compiled out of the server unless USE_IS2 is defined in config.h.
Without it the server rejects the IS2 keywords in the test configuration,
so this module asks the server to check the IS2 test configuration with
-y and ends the test with a skip line if the result is not what the test
needs.

=cut

use strict;
use warnings;

sub import
{
	my($class, $mode) = @_;
	my $want_enabled = (defined $mode && $mode eq 'disabled') ? 0 : 1;

	# only the aprsc binary is built with or without IS2
	return if (defined $ENV{'TEST_PRODUCT'} && $ENV{'TEST_PRODUCT'} !~ /aprsc/);
	return if (!-x '../src/aprsc');

	# a failure to run the check at all (a usage error, for example) must
	# not be mistaken for a rejected configuration: check the plain one too
	system('../src/aprsc -y -c cfg-aprsc/basic >/dev/null 2>&1');
	return if ($? != 0);

	system('../src/aprsc -y -c cfg-aprsc/is2-basic >/dev/null 2>&1');
	my $enabled = ($? == 0) ? 1 : 0;
	return if ($enabled == $want_enabled);

	print "1..0 # Skipped: IS2 is " . (($enabled) ? "enabled" : "not enabled")
		. " in this build of aprsc (USE_IS2 in config.h)\n";
	exit(0);
}

1;
