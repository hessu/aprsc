
package Ham::APRS::IS;

use 5.006;
use strict;
use warnings;

use Time::HiRes qw( time sleep );
use IO::Handle '_IOFBF';
use IO::Socket::INET;
use IO::Select;
use Data::Dumper;

our $VERSION = '0.01';

our $aprs_appid = "IS $VERSION";

our $debug = 0;

=head1 new(hostport, mycall, optionshash)

Initializes a new Ham::APRS::IS socket. Takes two mandatory arguments,
the host:port pair to connect to and your client's callsign, and one optional
argument, the filter string to be sent with the login command.

  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL');
  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL', 'filter' => 'f/*');
  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL', 'nopass' => 1);
  my $is = new Ham::APRS::IS('aprs.server.com:12345', 'N0CALL', 'udp' => 1);

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
	$self->{'udp'} = $options{'udp'} if (defined $options{'udp'});
	
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
	
	if ($self->{'udp'} && !defined $self->{'usock'}) {
		$self->{'usock'} = IO::Socket::INET->new(Proto => 'udp', LocalPort => $self->{'udp'});
		
		if (!defined($self->{'usock'})) {
			$self->{'error'} = "Failed to bind an UDP client socket: $@";
			return 0;
                }
                
                warn "bound udp port " . $self->{'udp'} . "\n";
	}
	
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
	
	my $s = sprintf("user %s pass %s vers %s",
			$self->{'mycall'},
			$self->{'aprspass'},
			$aprs_appid );
			
	if (defined($self->{'udp'})) {
		$s .= sprintf(" UDP %d",
			$self->{'udp'} );
	}
	
	if (defined($self->{'filter'})) {
		$s .= sprintf(" filter %s",
			$self->{'filter'} );
	}
	
	$s .= "\r\n";
	
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
		#warn "login got: $l\n";
		return 1 if ($l =~ /^#\s+logresp\s+/);
		if (time() - $t > 5) {
			$self->{'error'} = "Login command timed out";
			return 0;
		}
	}
	
	return 1;
}

=head1 connected()

Checks whether we're connected currently. Returns 1 for connected, 0 for not connected.

=cut

sub connected($)
{
	my($self) = @_;
	
	return 1 if $self->{'state'} eq 'connected';
	return 0;
}


=head1 accepted($socket)

Accepts a socket

=cut

sub accepted($$)
{
	my($self, $sock) = @_;
	
	$self->{'sock'} = $sock;
	$self->{'sock'}->blocking(0);
	$self->{'state'} = 'connected';
	$self->{'error'} = 'Accepted connection successfully';
	$self->{'ibuf'} = '';
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
	        # This really needs to check for a real CRLF sequence, not [\r\n]+.
	        # Otherwise the CRLF will once find itself on the boundary between
	        # two buffers / read() calls, return the line based on the \r alone,
	        # and then the next read will return an empty line before the \n.
		if ($self->{'ibuf'} =~ s/^(.*?)\r\n//s) {
			#warn "got: '$1'\n";
			return $1;
		}
		
		my($rin, $rout, $ein, $eout) = ('', '', '', '');
		my $rudp = '';
		vec($rin, fileno($sock), 1) = 1;
		my $rtcp = $rin;
		if (defined $self->{'usock'}) {
		        vec($rudp, fileno($self->{'usock'}), 1) = 1;
		        $rin |= $rudp;
		}
		$ein = $rtcp;
		my $nfound = select($rout = $rin, undef, $eout = $ein, $timeout);
		
		if ($nfound) {
			my $rbuf;
			if (defined $self->{'usock'} && (($rout & $rudp) eq $rudp)) {
				#warn "getline: got udp\n";
				my $msg;
				my $raddr = $self->{'usock'}->recv($msg, 1500);
				my($port, $ipaddr) = sockaddr_in($raddr);
				my $hishost = inet_ntoa($ipaddr);
				warn "got udp from $hishost $port: $msg\n";
				return $msg;
			}
			if (($rout & $rtcp) eq $rtcp) {
			        my $nread = sysread($sock, $rbuf, 1024);
			        if (!defined $nread || $nread < 1) {
			                $self->{'error'} = "Failed to read from server: $!";
			                warn "getline: read error (on read): $!\n";
			                $self->disconnect();
			                return undef;
                                } else {
                                        $self->{'ibuf'} .= $rbuf;
                                }
			}
			if (0 && $eout) {
			        $self->{'error'} = "Failed to read from server (select returned errors): $!";
			        warn "getline: read error (on select)\n";
			        $self->disconnect();
			        return undef;
                        }
		}
		
		if (time() > $end_t) {
			#warn "getline: timeout\n";
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

sub sendline($$;$$)
{
	my($self, $line, $raw, $noflush) = @_;
	
	warn "sendline $line\n" if ($debug);
	
	return undef if ($self->{'state'} ne 'connected');

	warn "blocking(1)\n" if ($debug);
	
	if (!defined $self->{'sock'}->blocking(1)) {
		warn "sendline: blocking(1) failed: $!\n";
		return undef;
	}
	
	warn "printf\n" if ($debug);
	
	if (!defined $self->{'sock'}) {
		warn "sendline: sock not defined: $!\n";
		return undef;
	}
	
	my $ret = $self->{'sock'}->printf( "%s%s", $line, ($raw) ? '' : "\r\n" );
	
	if (!$noflush) {
        	warn "flush\n" if ($debug);
        	
        	if (!defined $self->{'sock'}->flush) {
        		warn "sendline: flush() failed: $!\n";
        		return undef;
		}
	}
	
	if (!defined $self->{'sock'}->blocking(0)) {
		warn "sendline: blocking(1) failed: $!\n";
		return undef;
	}
	
	warn "sent ($ret): $line\n" if ($debug);
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

