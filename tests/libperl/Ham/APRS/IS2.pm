
package Ham::APRS::IS2;

use 5.006;
use strict;
use warnings;

use Time::HiRes qw( time sleep );
use IO::Handle '_IOFBF';
use IO::Socket::INET;
use IO::Select;
use Data::Dumper;

use Google::ProtocolBuffers;

Google::ProtocolBuffers->parsefile("../src/aprsis2.proto", { create_accessors => 1 });

our $VERSION = '0.01';

our $aprs_appname = "IS2";
our $aprs_appid = "$aprs_appname $VERSION";

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
	$self->{'loginstate'} = 'init';
	
	
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
	
	$self->{'loginstate'} = 'init';
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
			
			select(undef, undef, undef, 0.1);
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
	
	$self->{'state'} = 'connected';
	
	$self->{'sock'}->blocking(0);
	
	# wait for server signature
	my $t = time();
	while (my $l = $self->is2_frame_in()) {
		my $sig = $l->server_signature;
		
		if ($l->type == IS2Message::Type::SERVER_SIGNATURE()) {
			if (!$sig) {
				$self->{'error'} = "SERVER_SIGNATURE type, but no server signature message";
				return 0;
			}
			$self->{'loginstate'} = 'server_signature';
  			#warn sprintf("got server signature: serverid '%s' app '%s' version '%s'\n",
  			#	$sig->username, $sig->app_name, $sig->app_version);
			last;
		} else {
			$self->{'error'} = "Wrong type of message received instead of SERVER_SIGNATURE: " . $l->type;
			return 0;
		}
		
		if (time() - $t > 5) {
			$self->{'error'} = "Timed out waiting for server signature";
			return 0;
		}
	}
	
	if ($self->{'loginstate'} ne 'server_signature') {
		$self->{'error'} = "Timed out waiting for server signature";
		return 0;
	}
	
	# send login request
	my $lm = LoginRequest->new({
		'username' => $self->{'mycall'},
		'password' => $self->{'aprspass'},
		'app_name' => $aprs_appname,
		'app_version' => $VERSION
	});
	
	my $im = IS2Message->new({
		'type' => IS2Message::Type::LOGIN_REQUEST(),
		'login_request' => $lm
	});
	
	$self->{'sock'}->blocking(1);
	$self->is2_frame_out($im->encode);
	$self->{'sock'}->blocking(0);
	
	while (my $l = $self->is2_frame_in()) {
		my $rep = $l->login_reply;
		if ($l->type == IS2Message::Type::LOGIN_REPLY()) {
			if (!$rep) {
				$self->{'error'} = "LOGIN_REPLY type, but no login_reply message";
				return 0;
			}
			
			#warn sprintf("got login reply: result %d verified %d reason %d message '%s'\n",
			#	$rep->result, $rep->verified,
			#	defined $rep->result_code ? $rep->result_code : 0,
			#	defined $rep->result_message ? $rep->result_message : '');
			
			if ($rep->result != LoginReply::LoginResult::OK()) {
				$self->{'error'} = sprintf("Login reply: login failed, code %d: %s",
					defined $rep->result_code ? $rep->result_code : 0,
					defined $rep->result_message ? $rep->result_message : '');
				return 0;
			}
			
			if ($self->{'aprspass'} != -1 && $rep->verified < 1) {
				$self->{'error'} = sprintf("Login reply: login not verified (%d), code %d: %s",
					$rep->verified,
					defined $rep->result_code ? $rep->result_code : 0,
					defined $rep->result_message ? $rep->result_message : '');
				return 0;
			}
			
			return 1;
		} else {
			$self->{'error'} = "Wrong type of response received for LOGIN_REPLY: " . $l->type;
			return 0;
		}
		
		if (time() - $t > 5) {
			$self->{'error'} = "Login command timed out";
			return 0;
		}
	}
	
	$self->{'error'} = "No LOGIN_REPLY received";
	return 0;
}

sub send_packets($$)
{
	my($self, $packets) = @_;
	
	my @pq;
	foreach my $p (@{ $packets }) {
		push @pq, ISPacket->new({
			'type' => ISPacket::Type::IS_PACKET(),
			'is_packet_data' => $p
		});
	}
	
	my $im = IS2Message->new({
		'type' => IS2Message::Type::IS_PACKET(),
		'is_packet' => \@pq
	});
	
	$self->{'sock'}->blocking(1);
	$self->is2_frame_out($im->encode);
	$self->{'sock'}->blocking(0);
}

sub get_packets($;$)
{
	my($self, $timeout) = @_;
	
	my $t = time();
	
	while (my $l = $self->is2_frame_in($timeout)) {
		my $ips = $l->is_packet;
		if ($l->type == IS2Message::Type::IS_PACKET()) {
			if (!$ips) {
				$self->{'error'} = "IS_PACKET type, but no packets";
				return undef;
			}
			
			my @pa;
			
			foreach my $ip (@{ $ips }) {
				if ($ip->type != ISPacket::Type::IS_PACKET()) {
					$self->{'error'} = sprintf("ISPacket type %d unsupported", $ip->type);
					return undef;
				}
				
				push @pa, $ip->is_packet_data;
			}
			
			return @pa;
		} else {
			$self->{'error'} = "Wrong type of response received for LOGIN_REPLY: " . $l->type;
			return 0;
		}
		
		if (time() - $t > $timeout) {
			$self->{'error'} = "Login command timed out";
			return 0;
		}
	}
}


sub is2_frame_out($$)
{
	my($self, $frame) = @_;
	
	my $framelen = length($frame);
	
	if ($framelen >= 2**24) {
		$self->{'error'} = "Attempted to write too large frame: $framelen is over 2^24 bytes";
		return 0;
	} 
	
	my $framelen_i = pack('N', $framelen);
	
	#warn "is2_frame_out: framelen $framelen\n";
	
	if (!$self->{'sock'}->print(chr(0x02) . substr($framelen_i, 1) . $frame . chr(0x03))) {
		$self->{'error'} = "Failed to write IS2 frame to $self->{host_port}: $!";
		return 0;
	}
	
	if (!$self->{'sock'}->flush) {
		$self->{'error'} = "Failed to flush IS2 frame to $self->{host_port}: $!";
		return 0;
	}
}

sub is2_frame_in($;$)
{
	my($self, $timeout) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	
	$timeout = 5 if (!defined $timeout);
	
	my $end_t = time() + $timeout;
	my $sock = $self->{'sock'};
	
	while (1) {
		if (length($self->{'ibuf'}) >= 6) {
			if (substr($self->{'ibuf'}, 0, 1) ne chr(0x02)) {
				$self->{'error'} = "IS2 frame does not start with STX";
				#warn "is2_frame_in: " . $self->{'error'} . "\n";
				$self->disconnect();
				return undef;
			}
			my $frame_len_b = chr(0) . substr($self->{'ibuf'}, 1, 3);
			my $frame_len = unpack('N', $frame_len_b);
			#warn "frame len: $frame_len\n";
			my $need_bytes = $frame_len + 5;
			
			if (length($self->{'ibuf'}) >= $need_bytes) {
				my $etx = substr($self->{'ibuf'}, 4 + $frame_len, 1);
				if ($etx ne chr(0x03)) {
					$self->{'error'} = "IS2 frame does not end with ETX";
					#warn "is2_frame_in: " . $self->{'error'} . "\n";
					$self->disconnect();
					return undef;
				}
				
				my $frame = substr($self->{'ibuf'}, 4, $frame_len);
				$self->{'ibuf'} = substr($self->{'ibuf'}, $need_bytes);
				
				my $is2_msg = IS2Message->decode($frame);
				
				#warn "left in ibuf: " . length($self->{'ibuf'}) . "\n";
				return $is2_msg;
			}
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
			if (($rout & $rtcp) eq $rtcp) {
			        my $nread = sysread($sock, $rbuf, 1024);
			        if (!defined $nread || $nread < 1) {
			                $self->{'error'} = "Failed to read from server: $!";
			                #warn "is2_frame_in: read error (on read): $!\n";
			                $self->disconnect();
			                return undef;
                                } else {
                                        $self->{'ibuf'} .= $rbuf;
                                }
			}
			if (0 && $eout) {
			        $self->{'error'} = "Failed to read from server (select returned errors): $!";
			        #warn "is2_frame_in: read error (on select)\n";
			        $self->disconnect();
			        return undef;
                        }
		}
		
		if (time() > $end_t) {
			#warn "is2_frame_in: timeout\n";
			return undef;
		}
	}
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

