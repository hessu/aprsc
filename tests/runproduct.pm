
package runproduct;

=head1 NAME

runproduct - Runs and stops either aprsc or javaprssrvr with a selected
configuration, hiding the application-specific details from the test.

=cut

use 5.006;
use strict;
use warnings;
use IPC::Open3;
use POSIX ":sys_wait_h";
use Data::Dumper;

my %products = (
	'aprsc' => {
		'binary' => '../src/aprsc',
		'stdargs' => '',
		'cfgfileargs' => '-c',
		'cfgdir' => 'cfg-aprsc'
	},
	'javap' => {
		'binary' => '/usr/bin/java',
		'stdargs' => '-server -cp ../../../javaprssrvr/javAPRSSrvr.jar javAPRSSrvr',
		'cfgfileargs' => '',
		'cfgdir' => 'cfg-javap'
	}
);

my $prod;
my $prod_name = 'aprsc';
my $pid;
my($stdin, $stdout, $stderr);

sub init()
{
	$prod_name = $ENV{'TEST_PRODUCT'} if (defined $ENV{'TEST_PRODUCT'});
	
	if (!defined $products{$prod_name}) {
		warn "No such product: " . $prod_name . "\n";
		return undef;
	}
	
	$prod = $products{$prod_name};
	
	return 1;
}

sub readout()
{
	if (defined $stderr) {
	}
	
	if (defined $stdout) {
	}
}

sub start($)
{
	my($config) = @_;
	
	if (defined $pid) {
		return "Product already running.";
	}
	
	my $cfgfile = $prod->{'cfgdir'} . '/' . $config;
	if (! -f $cfgfile) {
		return "No such configuration file: $cfgfile";
	}
	
	my $cmdline = $prod->{'binary'} . ' ' . $prod->{'stdargs'} . ' '
		. $prod->{'cfgfileargs'} . ' ' . $cfgfile;
	
	warn "Product command line: $cmdline\n";
	
	$pid = open3($stdin, $stdout, $stderr, $cmdline);
	
	if (!defined $pid) {
		return "Failed to run product: $!";
	}
	
	# let it start...
	sleep(1);
	
	my $kid = waitpid($pid, WNOHANG);
	
	if ($kid) {
		my $retval = $?;
		my $signal = $retval & 127;
		$retval = $retval >> 8;
		
		readout();
		discard();
		return "Product quit after startup, signal $signal retcode $retval.";
	}
	
	return 1;
}

sub discard()
{
	close($stdin) if (defined $stdin);
	close($stdout) if (defined $stdout);
	close($stderr) if (defined $stderr);
	undef $pid;
}

sub stop()
{
	if (!defined $pid) {
		return "Product not running.";
	}
	
	my $kid = waitpid($pid, WNOHANG);
	
	if ($kid) {
		my $retval = $?;
		my $signal = $retval & 127;
		$retval = $retval >> 8;
		
		readout();
		discard();
		return "Product has crashed, signal $signal retcode $retval.";
	}
	
	my $hits = kill("TERM", $pid);
	if ($hits < 1) {
		return "Product is not running.";
		discard();
		return undef;
	}
	
	my $sleeptime = 0.2;
	my $maxwait = 5;
	my $slept = 0;
	my $rekilled = 0;
	while (!($kid = waitpid($pid, WNOHANG))) {
		$slept += select(undef, undef, undef, $sleeptime);
		if ($slept >= $maxwait) {
			if ($rekilled) {
				return "Product refuses to die!";
			} else {
				$slept = 0;
				$rekilled = 1;
				kill("KILL", $pid);
			}
		}
	}
	
	if ($kid) {
		my $retval = $?;
		my $signal = $retval & 127;
		$retval = $retval >> 8;
		
		readout();
		discard();
		if ($retval ne 0 || $signal ne 0) {
			return "Product has been terminated, signal $signal retcode $retval.";
		}
	}
	
	discard();
	return 1;
}

1;
