
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

my $debug = 0;

my %products = (
	'aprsc' => {
		'binary' => '../src/aprsc',
		'stdargs' => '-e debug -o file -r logs',
		'cfgfileargs' => '-c',
		'cfgdir' => 'cfg-aprsc',
		'pidfile' => 'logs/aprsc.pid'
		
	},
	'javap' => {
		'binary' => './javaprssrvr/java',
		'stdargs' => '-server -cp ./javaprssrvr/javAPRSSrvr.jar javAPRSSrvr',
		'cfgfileargs' => '',
		'cfgdir' => 'cfg-javap',
		'dieswith' => 15,
		'exitcode' => 143
	},
	'javap4' => {
		'binary' => '../javaprssrvr/java',
		'chdir' => './javaprssrvr4',
		'stdargs' => '-server -cp javAPRSSrvr.jar SrvrMain',
		'cfgfileargs' => '',
		'cfgdir' => '.',
		'dieswith' => 15,
		'exitcode' => 143
	}
);

sub new($$)
{
	my($class, $config) = @_;
	my $self = bless { @_ }, $class;
	
	if (defined $ENV{'TEST_PRODUCT'}) {
		$self->{'prod_name'} = $ENV{'TEST_PRODUCT'};
	} else {
		$self->{'prod_name'} = 'aprsc';
	}
	
	if (!defined $products{$self->{'prod_name'}}) {
		warn "No such product: " . $self->{'prod_name'} . "\n";
		return undef;
	}
	
	my $prod = $self->{'prod'} = $products{$self->{'prod_name'}};
	
	if (defined $self->{'prod'}->{'chdir'}) {
		chdir($self->{'prod'}->{'chdir'}) || return "could not chdir to " . $self->{'prod'}->{'chdir'};
	}
	
	my $cfgfile = $self->{'cfgfile'} = $prod->{'cfgdir'} . '/' . $config;
	if (! -f $cfgfile) {
		warn "No such configuration file: $cfgfile";
		return undef;
	}
	
	$self->{'cmdline'} = $prod->{'binary'} . ' ' . $prod->{'stdargs'} . ' '
		. $prod->{'cfgfileargs'} . ' ' . $cfgfile;
	
	$self->{'error'} = 'No errors yet';
	
	return $self;
}

sub readout($)
{
	my($self) = @_;
}

sub start($)
{
	my($self) = @_;
	
	if (defined $self->{'pid'}) {
		return "Product already running.";
	}
	
	if (defined $self->{'prod'}->{'pidfile'}) {
		my $pf = $self->{'prod'}->{'pidfile'};
		if (open(PF, $pf)) {
			my $pl = <PF>;
			close(PF);
			if ($pl =~ /^(\d+)/) {
				#warn "runproduct: found old pid $1 from pidfile\n";
				if (kill(9, $1)) {
					warn "runproduct: killed old process $1 based on pid file\n";
					sleep(1); # let it die
				}
			}
		}
	}
	
	#warn "Product command line: $self->{cmdline}\n";
	
	my($stdin, $stdout, $stderr);
	my $pid = open3($stdin, $stdout, $stderr, $self->{'cmdline'});
	
	#if (defined $self->{'prod'}->{'chdir'}) {
	#	chdir('..');
	#}
	
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
		
		$self->readout();
		$self->discard();
		return "Product quit after startup, signal $signal retcode $retval.";
	}
	
	$self->{'pid'} = $pid;
	$self->{'stdin'} = $stdin;
	$self->{'stdout'} = $stdout;
	$self->{'stderr'} = $stderr;
	
	warn "\nproduct started, pid $pid\n" if ($debug);
	
	return 1;
}

sub discard($)
{
	my($self) = @_;
	
	close($self->{'stdin'}) if (defined $self->{'stdin'});
	close($self->{'stdout'}) if (defined $self->{'stdout'});
	close($self->{'stderr'}) if (defined $self->{'stderr'});
	
	undef $self->{'stdin'};
	undef $self->{'stdout'};
	undef $self->{'stderr'};
	undef $self->{'pid'};
}

sub check($)
{
	my($self) = @_;
	
	if (!defined $self->{'pid'}) {
		return "Product not running.";
	}
	
	my $kid = waitpid($self->{'pid'}, WNOHANG);
	
	if ($kid) {
		my $retval = $?;
		my $signal = $retval & 127;
		$retval = $retval >> 8;
		
		$self->readout();
		$self->discard();
		
		return "Product has crashed, signal $signal retcode $retval.";
	}
	
	return 1;
}

sub stop($)
{
	my($self) = @_;
	
	my $ret = $self->check();
	return $ret if ($ret ne 1);
	
	my $pid = $self->{'pid'};
	
	warn "\nkilling product, pid $pid\n" if ($debug);
	
	my $hits = kill("TERM", $pid);
	if ($hits < 1) {
		warn "\nkilling did not hit anything - not running, pid $pid\n" if ($debug);
		return "Product is not running.";
		$self->discard();
		return undef;
	}
	
	my $sleeptime = 0.2;
	my $maxwait = 6;
	my $slept = 0;
	my $rekilled = 0;
	my $kid;
	while (!($kid = waitpid($pid, WNOHANG))) {
		select(undef, undef, undef, $sleeptime);
		$slept += $sleeptime;
		if ($slept >= $maxwait) {
			if ($rekilled) {
				warn "\nproduct refuses to die, pid $pid\n" if ($debug);
				return "Product refuses to die!";
			} else {
				warn "Sending SIGKILL to $pid...\n";
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
		
		$self->readout();
		$self->discard();
		if ($retval ne 0 || $signal ne 0) {
			if (defined $self->{'prod'}->{'exitcode'} && $self->{'prod'}->{'exitcode'} eq $retval) {
				warn "\nproduct kill: ok, retval match $retval, pid $pid\n" if ($debug);
				# fine
			} elsif (defined $self->{'prod'}->{'dieswith'} && $self->{'prod'}->{'dieswith'} eq $signal) {
				warn "\nproduct kill: ok, dieswith signal match $retval, pid $pid\n" if ($debug);
				# fine
			} else {
				warn "\nproduct kill: ok\n" if ($debug);
				return "Product has been terminated, signal $signal retcode $retval.";
			}
		}
	}
	
	warn "\nproduct kill: end, pid $pid\n" if ($debug);
	
	$self->discard();
	return 1;
}

1;
