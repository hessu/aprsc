
package Ham::APRS::IS;

use 5.006;
use strict;
use warnings;

use Time::HiRes qw( time sleep );
use IO::Handle '_IOFBF';
use IO::Socket::INET;
use IO::Select;

our $VERSION = '0.01';

our $aprs_appid = "IS $VERSION";

=head1 new(hostport, mycall, optionshash)

Initializes a new Ham::APRS::IS socket. Takes two mandatory arguments,
the host:port pair to connect to and your client's callsign, and one optional
argument, the filter string to be sent with the login command.

  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL');
  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL', 'filter' => 'f/*');
  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL', 'nopass' => 1);

=cut

sub new($$$;%)
{
	my $that = shift;
	my $class = ref($that) || $that;
	my $self = { };
	bless ($self, $class);
	
	my($host_port, $mycall, %options) = @_;
	
	$self->{'host_port'} = $host_port;
	$self->{'mycall'} = $mycall;
	$self->{'filter'} = $options{'filter'} if (defined $options{'filter'});
	
	if ($options{'nopass'}) {
		$self->{'aprspass'} = -1;
	} else {
		$self->{'aprspass'} = aprspass($self->{'mycall'});
	}
	
	#warn "aprspass for $self->{mycall} is $self->{aprspass}\n";
	
	$self->{'state'} = 'init';
	$self->{'error'} = "No errors yet.";
	
	return $self;
}

=head1 disconnect()

Disconnects from the server. Returns 1 on success, 0 on failure.

  $is->disconnect() || die "Failed to disconnect: $is->{error}";

=cut

sub disconnect($)
{
	my($self) = @_;
	
	if (defined $self->{'sock'}) {
		$self->{'sock'}->close;
		undef $self->{'sock'};
	}
	
	$self->{'state'} = 'disconnected';
	
	return 1;
}

=head1 connect(options)

Connects to the server. Returns 1 on success, 0 on failure.
Takes an optional options hash as a parameter. Currently knows only one parameter,
retryuntil, which specifies the number of seconds to retry the connection. After
each failed attempt the code sleeps for 0.5 seconds before trying again. Defaults
to 0 (no retries).

  $is->connect('retryuntil' => 10) || die "Failed to connect: $is->{error}";

=cut

sub connect($;%)
{
	my($self) = shift;
	
	my %options = @_;
	
	if ($self->{'state'} eq 'connected') {
		$self->{'error'} = 'Already connected';
		return 0;
	}
	
	$self->{'ibuf'} = '';
	
	my $retryuntil = defined $options{'retryuntil'} ? $options{'retryuntil'} : 0;
	my $starttime = time();
	
	while (!defined $self->{'sock'}) {
		$self->{'sock'} = IO::Socket::INET->new($self->{'host_port'});
		
		if (!defined($self->{'sock'})) {
			$self->{'error'} = "Failed to connect to $self->{host_port}: $!";
			
			if (time() - $starttime >= $retryuntil) {
				return 0;
			}
			
			select(undef, undef, undef, 0.5);
		}
	}
	
	$self->{'error'} = 'Connected successfully';
	
	#   printf ( "APRS::IS->new()  mycall='%s'  aprspass=%d   filterre='%s'\n",
	#            $self->{aprsmycall}, $self->{aprspass}, $self->{filterre} );
	
	##
	##    *  Need to send on initial connect the following logon line:
	##      user callsign pass passcode vers appname versionnum rest_of_line
	##
	##      callsign = login callsign-SSID
	##      passcode = login passcode per APRS-IS algorithm, -1 = read-only
	##      appname = application name (1 word)
	##      versionnum = application version number (no spaces)
	##      rest_of_line = server command if connecting to a port that supports commands (see Server Commands)
	##
	##      (appname and versionnum should not exceed 15 characters)
	##
	##       
	##    * Need to recognize both TCPIP and TCPXX as TCP/IP stations
	##    * Need to provide a means to perform the user validation. This can either be a user entered password,
	##      or a client program can automatically figure out the password given the callsign.
	##      If the later is used, it is the client programmer's responsibility to be certain that non-amateurs
	##      are not given registrations that can validate themselves in APRS-IS.
	##    * Probably a good idea to perform some feedback about the limitations of TCPIP without a registration number.
	##
	
	$self->{'sock'}->blocking(1);
	$self->{'state'} = 'connected';
	
	my $s;
	if (defined($self->{'filter'})) {
		$s = sprintf("user %s pass %s vers %s filter %s\r\n",
			$self->{'mycall'},
			$self->{'aprspass'}, # -- but we are read-only !
			$aprs_appid, $self->{'filter'} );
	} else {
		$s = sprintf("user %s pass %s vers %s\r\n",
			$self->{'mycall'},
			$self->{'aprspass'}, # -- but we are read-only !
			$aprs_appid );
	}
	
	#warn "login: $s\n";
	if (!$self->{'sock'}->print($s)) {
		$self->{'error'} = "Failed to write login command to $self->{host_port}: $!";
		return 0;
	}
	
	if (!$self->{'sock'}->flush) {
		$self->{'error'} = "Failed to flush login command to $self->{host_port}: $!";
		return 0;
	}
	
	$self->{'sock'}->blocking(0);
	
	my $t = time();
	while (my $l = $self->getline()) {
		return 1 if ($l =~ /^#\s+logresp\s+/);
		if (time() - $t > 5) {
			$self->{'error'} = "Login command timed out";
			return 0;
		}
	}
	
	return 1;
}

# -------------------------------------------------------------------------
# Get a line (blocking)

sub getline($;$)
{
	my($self, $timeout) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	
	$timeout = 5 if (!defined $timeout);
	
	my $end_t = time() + $timeout;
	my $sock = $self->{'sock'};
	
	while (1) {
		if ($self->{'ibuf'} =~ s/^(.*?)[\r\n]+//s) {
			#warn "got: $1\n";
			return $1;
		}
		
		if (time() > $end_t) {
			warn "getline: timeout\n";
			return undef;
		}
		
		my($rin, $rout, $ein, $eout) = ('', '', '', '');
		vec($rin, fileno($sock), 1) = 1;
		$ein = $rin;
		my $nfound = select($rout = $rin, undef, $eout = $ein, 1);
		
		if (($nfound) && ($rout)) {
			my $rbuf;
			my $nread = sysread($sock, $rbuf, 1024);
			if ($nread > 0) {
				$self->{'ibuf'} .= $rbuf;
			} elsif ($nread < 1) {
				$self->{'error'} = "Failed to read from server: $!";
				warn "getline: read error (on read)\n";
				$self->disconnect();
				return undef;
			}
		} elsif (($nfound) && ($eout)) {
			$self->{'error'} = "Failed to read from server (select returned errors): $!";
			warn "getline: read error (on select)\n";
			$self->disconnect();
			return undef;
		}
	}
}

sub getline_noncomment($;$)
{
	my($self, $timeout) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	while (my $l = $self->getline($timeout)) {
		return $l if !defined $l;
		return $l if ($l !~ /^#/);
	}
}

sub sendline($$)
{
	my($self, $line) = @_;
	return undef if ($self->{'state'} ne 'connected');
	
	if (!defined $self->{'sock'}->blocking(1)) {
		warn "sendline: blocking(1) failed: $!\n";
		return undef;
	}
	my $ret = $self->{'sock'}->printf( "%s\r\n", $line);
	if (!$self->{'sock'}->flush) {
		warn "sendline: flush() failed: $!\n";
		return undef;
	}
	
	if (!defined $self->{'sock'}->blocking(0)) {
		warn "sendline: blocking(1) failed: $!\n";
		return undef;
	}
	
	#warn "sent ($ret): $line\n";
	return $ret;
}

=head1 aprspass($callsign)

Calculates the APRS passcode for a given callsign. Ignores SSID
and converts the callsign to uppercase as required. Returns an integer.

  my $passcode = Ham::APRS::IS::aprspass($callsign);

=cut

sub aprspass($)
{
	my($call) = @_;
	
	$call =~ s/-([^\-]+)$//;
	$call = uc($call);
	
	my ($a, $h) = (0, 0);
	map($h ^= ord(uc) << ($a^=8), $call =~ m/./g);
	return (($h ^ 29666) & 65535);
}


1;

