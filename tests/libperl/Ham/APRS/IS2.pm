
package Ham::APRS::IS2;

use 5.006;
use strict;
use warnings;

use Time::HiRes qw( time sleep );
use IO::Handle '_IOFBF';
use IO::Socket::INET;
use IO::Select;
use Data::Dumper;
use Scalar::Util qw( blessed );

use Google::ProtocolBuffers::Dynamic;

my $pf = Google::ProtocolBuffers::Dynamic->new("../src");
$pf->load_file("aprsis2.proto");
$pf->map({package => "aprsis2", prefix => "APRSIS2"});

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
	$self->{'udp-peer'} = $options{'udp-peer'} if (defined $options{'udp-peer'});
	
	if ($options{'nopass'}) {
		$self->{'aprspass'} = -1;
	} else {
		$self->{'aprspass'} = aprspass($self->{'mycall'});
	}
	
	#warn "aprspass for $self->{mycall} is $self->{aprspass}\n";
	
	$self->{'state'} = 'init';
	$self->{'error'} = "No errors yet.";
	$self->{'loginstate'} = 'init';
	
	$self->{'pqueue_in'} = [];
	
	return $self;
}

=head1 disconnect()

Disconnects from the server. Returns 1 on success, 0 on failure.

  $is->disconnect() || die "Failed to disconnect: $is->{error}";

=cut

sub disconnect($)
{
	my($self) = @_;
	
	if (defined $self->{'usock'}) {
		return 0;
	}

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
	
	if (!$self->wait_signature()) {
		return 0;
	}
	
	$self->{'loginstate'} = 'server_signature';
	
	if ($self->{'loginstate'} ne 'server_signature') {
		$self->{'error'} = "Timed out waiting for server signature";
		return 0;
	}
	
	# send login request
	my $lm = APRSIS2::IS2LoginRequest->new({
		'username' => $self->{'mycall'},
		'password' => $self->{'aprspass'},
		'app_name' => $aprs_appname,
		'app_version' => $VERSION
	});
	
	my $im = APRSIS2::IS2Message->new({
		'type' => APRSIS2::IS2Message::Type::LOGIN_REQUEST(),
		'login_request' => $lm
	});
	
	$self->is2_frame_out($im->encode);
	
	my $t = time();
	while (my $l = $self->is2_frame_in()) {
		if ($l->get_type() == APRSIS2::IS2Message::Type::LOGIN_REPLY()) {
			my $rep = $l->get_login_reply();
			if (!$rep) {
				$self->{'error'} = "LOGIN_REPLY type, but no login_reply message";
				return 0;
			}
			
			#warn sprintf("got login reply: result %d verified %d reason %d message '%s'\n",
			#	$rep->result, $rep->verified,
			#	defined $rep->result_code ? $rep->result_code : 0,
			#	defined $rep->result_message ? $rep->result_message : '');
			
			if ($rep->get_result() != APRSIS2::IS2LoginReply::LoginResult::OK()) {
				$self->{'error'} = sprintf("Login reply: login failed, code %d: %s",
					defined $rep->get_result_code() ? $rep->get_result_code() : 0,
					defined $rep->get_result_message() ? $rep->get_result_message() : '');
				return 0;
			}
			
			if ($self->{'aprspass'} != -1 && $rep->get_verified() < 1) {
				$self->{'error'} = sprintf("Login reply: login not verified (%d), code %d: %s",
					$rep->get_verified(),
					defined $rep->get_result_code() ? $rep->get_result_code() : 0,
					defined $rep->get_result_message() ? $rep->get_result_message() : '');
				return 0;
			}
			
			if (defined $self->{'filter'}) {
				return $self->set_filter($self->{'filter'});
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
		if (defined blessed($p) && blessed($p) eq 'APRSIS2::ISPacket') {
			push @pq, $p;
		} else {
			push @pq, APRSIS2::ISPacket->new({
				'type' => APRSIS2::ISPacket::Type::IS_PACKET(),
				'is_packet_data' => $p
			});
		}
	}
	
	my $im = APRSIS2::IS2Message->new({
		'type' => APRSIS2::IS2Message::Type::IS_PACKET(),
		'is_packet' => \@pq
	});
	
	return $self->is2_frame_out($im->encode);
}

sub wait_signature($)
{
	my($self) = @_;
	
	# wait for server signature
	my $t = time();
	while (my $l = $self->is2_frame_in()) {
		my $sig = $l->get_server_signature();
		
		if ($l->get_type() == APRSIS2::IS2Message::Type::SERVER_SIGNATURE()) {
			if (!$sig) {
				$self->{'error'} = "SERVER_SIGNATURE type, but no server signature message";
				return 0;
			}
			return 1;
  			#warn sprintf("got server signature: serverid '%s' app '%s' version '%s'\n",
  			#	$sig->username, $sig->app_name, $sig->app_version);
		} else {
			$self->{'error'} = "Wrong type of message received instead of SERVER_SIGNATURE: " . $l->get_type();
			return 0;
		}
		
		if (time() - $t > 5) {
			$self->{'error'} = "Timed out waiting for server signature";
			return 0;
		}
	}
	
	$self->{'error'} = "Failed to read server signature: $!";
	return 0;
}

sub ping($)
{
	my($self) = @_;
	
	my $timeout = 2;
	my $reqid = int(rand(2**30));
	
	my $im = APRSIS2::IS2Message->new({
		'type' => APRSIS2::IS2Message::Type::KEEPALIVE_PING(),
		'keepalive_ping' => APRSIS2::IS2KeepalivePing->new({
			'ping_type' => APRSIS2::IS2KeepalivePing::PingType::REQUEST(),
			'request_id' => $reqid,
		})
	});
	
	$self->is2_frame_out($im->encode);
	
	my $t = time();
	
	while (my $l = $self->is2_frame_in($timeout)) {
		if ($l->get_type() != APRSIS2::IS2Message::Type::KEEPALIVE_PING()) {
			warn "IS2 ping: Unexpected type of frame received: " . $l->get_type() . "\n";
			next;
		}
		
		my $ping = $l->get_keepalive_ping();
		if (!defined $ping) {
			$self->{'error'} = "IS2 ping reply does not have keepalive_ping payload";
			return undef;
		}
		
		if ($ping->get_ping_type() != APRSIS2::IS2KeepalivePing::PingType::REPLY()) {
			$self->{'error'} = "IS2 ping: Wrong type of ping frame received: " . $ping->get_ping_type();
			return undef;
		}
		
		if ($ping->get_request_id() != $reqid) {
			$self->{'error'} = "IS2 ping: Request id mismatch, sent $reqid, got " . $ping->get_request_id();
			return undef;
		}
		
		return 1;
	}
	
	$self->{'error'} = "ping get_packets timed out";
	return undef;
}

sub send_packet($)
{
	my($self, $packet) = @_;

	return undef if ($self->{'state'} ne 'connected');

	$self->send_packets([$packet]);

	return 1;
}

sub get_packets($;$$$)
{
	my($self, $timeout, $dupes, $is2) = @_;
	
	my $t = time();
	
	my $required_type = ($dupes) ? APRSIS2::ISPacket::Type::IS_PACKET_DUPLICATE() : APRSIS2::ISPacket::Type::IS_PACKET();
	
	while (my $l = $self->is2_frame_in_nonping($timeout)) {
		if ($l->get_type() == APRSIS2::IS2Message::Type::IS_PACKET()) {
			my $ips = $l->get_is_packet_list();
			if (!$ips) {
				$self->{'error'} = "IS_PACKET type, but no packets";
				return undef;
			}
			
			my @pa;
			
			foreach my $ip (@{ $ips }) {
				if ($ip->get_type() != $required_type) {
					$self->{'error'} = sprintf("ISPacket type %d not expected", $ip->get_type());
					return undef;
				}

				if ($is2) {
					push @pa, $ip;
				} else {
					my $pd = $ip->get_is_packet_data();
					if (!defined $pd) {
						$self->{'error'} = "ISPacket with no packet data received";
						return undef;
					}
					push @pa, $pd;
				}
			}
			
			return @pa;
		} else {
			$self->{'error'} = "Wrong type of frame received: " . $l->get_type();
			return undef;
		}
		
		if (time() - $t > $timeout) {
			$self->{'error'} = "get_packets timed out";
			return undef;
		}
	}
	warn "get_packets got undef with is2_frame_in_nonping\n";
	return undef;
}

sub get_packet($;$$$)
{
	my($self, $timeout, $duplicates, $is2) = @_;
	
	if (@{ $self->{'pqueue_in'} }) {
		if ($is2) {
			return shift @{ $self->{'pqueue_in'} };
		} else {
			my $ip = shift @{ $self->{'pqueue_in'} };
			my $pd = $ip->get_is_packet_data();
			if (!defined $pd) {
				$self->{'error'} = "ISPacket with no packet data received";
				return undef;
			}
			return $pd;
		}
	}
	
	my @p = $self->get_packets($timeout, $duplicates, 1);
	
	if (@p) {
		my $ip = shift @p;
		$self->{'pqueue_in'} = \@p;
		if ($is2) {
			return $ip;
		} else {
			if (!defined $ip) {
				return undef;
			}
			my $pd = $ip->get_is_packet_data();
			if (!defined $pd) {
				$self->{'error'} = "ISPacket with no packet data received";
				return undef;
			}
			return $pd;
		}
	}
	
	return;
}

sub set_filter($$)
{
	my($self, $filter) = @_;
	
	my $reqid = int(rand(2**30));
	
	my $im = APRSIS2::IS2Message->new({
		'type' => APRSIS2::IS2Message::Type::PARAMETER(),
		'parameter' => APRSIS2::IS2Parameter->new({
			'type' => APRSIS2::IS2Parameter::Type::PARAMETER_SET(),
			'request_id' => $reqid, # todo: sequential
			'filter_string' => $filter
		})
	});
	
	$self->is2_frame_out($im->encode);
	
	my $t = time();
	while (my $l = $self->is2_frame_in()) {
		if ($l->get_type() == APRSIS2::IS2Message::Type::PARAMETER()) {
			my $rep = $l->get_parameter();
			if (!$rep) {
				$self->{'error'} = "PARAMETER type, but no parameter message";
				return 0;
			}
			
			if ($rep->get_request_id() != $reqid) {
				$self->{'error'} = "PARAMETER reply, wrong request id " . $rep->get_request_id() . ", expected $reqid";
				return 0;
			}
			
			if ($rep->get_type() != APRSIS2::IS2Parameter::Type::PARAMETER_APPLIED()) {
				$self->{'error'} = sprintf("filter set reply: not applied");
				return 0;
			}
			
			# todo: check sequence
			return 1;
		} else {
			$self->{'error'} = "Wrong type of response received for PARAMETER_SET: " . $l->get_type();
			return 0;
		}
		
		if (time() - $t > 5) {
			$self->{'error'} = "parameter command timed out";
			return 0;
		}
	}
	
	$self->{'error'} = "No PARAMETER_APPLIED received";
	return 0;
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
	
	my $is2_framed = chr(0x02) . substr($framelen_i, 1) . $frame . chr(0x03);

	if ($self->{'usock'}) {
		my $udp_frame_len = length($is2_framed);
		if ($udp_frame_len > 1400) {
			$self->{'error'} = "Attempted to write too large UDP frame: $udp_frame_len is over 1400 bytes";
			return 0;
		}
		if (!$self->{'usock'}->send($is2_framed, 0, $self->{'dest'})) {
			$self->{'error'} = "Failed to write IS2 frame to UDP socket: $!";
			return 0;
		}
	} else {
		$self->{'sock'}->blocking(1);
		if (!$self->{'sock'}->print($is2_framed)) {
			$self->{'error'} = "Failed to write IS2 frame to $self->{host_port}: $!";
			$self->{'sock'}->blocking(0);
			return 0;
		}
		
		if (!$self->{'sock'}->flush) {
			$self->{'error'} = "Failed to flush IS2 frame to $self->{host_port}: $!";
			$self->{'sock'}->blocking(0);
			return 0;
		}
		$self->{'sock'}->blocking(0);
	}
	return 1;
}

sub is2_frame_in($;$)
{
	my($self, $timeout) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	
	$timeout = 5 if (!defined $timeout);

	if (defined $self->{'usock'}) {
		return $self->is2_udp_frame_in($timeout);
	}
	
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
				
				my $is2_msg = APRSIS2::IS2Message->decode($frame);
				#warn "decoded: " . Dumper($is2_msg) . "\n";
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
			$self->{'error'} = "is2_frame_in timed out";
			return undef;
		}
	}
}

sub udp_frame_in($$)
{
	my($self, $timeout) = @_;

	my $end_t = time() + $timeout;
	my $sock = $self->{'usock'};
	
	while (1) {
		my($rin, $rout, $ein, $eout) = ('', '', '', '');
		vec($rin, fileno($self->{'usock'}), 1) = 1;
		my $nfound = select($rout = $rin, undef, $eout = $ein, $timeout);
		
		if ($nfound) {
			my $rbuf;
			if (($rout & $rin) eq $rin) {
				#warn "getline: got udp\n";
				my $msg;
				my $raddr = $self->{'usock'}->recv($msg, 1500);
				my($port, $ipaddr) = sockaddr_in($raddr);
				my $hishost = inet_ntoa($ipaddr);
				#warn "got udp from $hishost $port: $msg\n";
				return $msg;
			}
		}
		
		if (time() > $end_t) {
			#warn "getline: timeout\n";
			return undef;
		}
	}
}

sub is2_udp_frame_in($$)
{
	my($self, $timeout) = @_;

	my $msg = $self->udp_frame_in($timeout);
	if (defined $msg) {
		if (substr($msg, 0, 2) eq "# ") {
			return $self->is2_udp_corepeer_control_in($msg);
		} else {
			return $self->is2_udp_frame_decode($msg);
		}
	}
	return $msg;
}

sub is2_udp_corepeer_control_in($$)
{
	my($self, $msg) = @_;

	#warn "is2_udp_corepeer_control_in: $msg (end)\n";
	# PEER2 v 2 hello TESTING vers aprsc 2.1.15-gfc61e9fM token eu068neq4f
	# PEER2 v 2 ok TESTING vers aprsc 2.1.15-gfc61e9fM token hq736t7q63
	if ($msg =~ /^# PEER2 v 2 ([^ ]+) ([^ ]+) vers ([^ ]+) ([^ ]+) token ([^ ]+)$/) {
		my($operation, $peer_call, $peer_soft, $peer_vers, $rx_token) = ($1, $2, $3, $4, $5);

		if ($operation eq "ok") {
			#warn "is2_udp_corepeer_control_in PEER2 ok token $rx_token";
			$self->{'corepeer-ok-rx'} = 1;
		} elsif ($operation eq "hello") {
			#warn "is2_udp_corepeer_control_in PEER2 hello token $rx_token, responding";
			$self->{'corepeer-ok-tx'} = 1;
		        my $hello = "# PEER2 v 2 ok " . $self->{'mycall'} . " vers perl-is1 1.0 token $rx_token";
			if (!$self->send_udp_packet($hello)) {
				return undef;
			}
		} else {
			warn "is2_udp_corepeer_control_in unknown PEER2 operation: $operation token $rx_token\n";
		}
		if (defined $self->{'corepeer-ok-tx'} && defined $self->{'corepeer-ok-rx'}) {
			#warn "is2_udp_corepeer_control_in is LINKED\n";
		}
	} else {
		warn "unrecognized control message\n";
	}
	return undef;
}

sub is2_udp_frame_decode($$)
{
	my($self, $msg) = @_;

	if (length($msg) < 6) {
		$self->{'error'} = "IS2 UDP frame too short";
		warn "is2_udp_frame_in: " . $self->{'error'} . "\n";
		$self->disconnect();
		return undef;
	}
	if (substr($msg, 0, 1) ne chr(0x02)) {
		$self->{'error'} = "IS2 UDP frame does not start with STX";
		warn "is2_udp_frame_in: " . $self->{'error'} . "\n";
		$self->disconnect();
		return undef;
	}
	my $frame_len_b = chr(0) . substr($msg, 1, 3);
	my $frame_len = unpack('N', $frame_len_b);
	#warn "frame len: $frame_len\n";
	my $need_bytes = $frame_len + 5;

	if (length($msg) < $need_bytes) {
		$self->{'error'} = "IS2 UDP frame too short for message length";
		#warn "is2_udp_frame_in: " . $self->{'error'} . "\n";
		$self->disconnect();
		return undef;
	}

	my $etx = substr($msg, 4 + $frame_len, 1);
	if ($etx ne chr(0x03)) {
		$self->{'error'} = "IS2 UDP frame does not end with ETX";
		#warn "is2_udp_frame_in: " . $self->{'error'} . "\n";
		$self->disconnect();
		return undef;
	}
	
	my $frame = substr($msg, 4, $frame_len);
	
	my $is2_msg = APRSIS2::IS2Message->decode($frame);
	#warn "is2_udp_frame_in decoded: " . Dumper($is2_msg) . "\n";
	return $is2_msg;
}

sub is2_frame_in_nonping($;$)
{
	my($self, $timeout) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	
	$timeout = 5 if (!defined $timeout);
	
	my $end_t = time() + $timeout;
	while (1) {
		my $l = $self->is2_frame_in($timeout);
		return $l if (!defined $l);

		if ($l->get_type() == APRSIS2::IS2Message::Type::KEEPALIVE_PING()) {
			warn "received ping, replying\n";
			my $ping = $l->get_keepalive_ping();
			if (!defined $ping) {
				$self->{'error'} = "IS2 ping does not have keepalive_ping payload";
				return undef;
			}
			
			if ($ping->get_ping_type() != APRSIS2::IS2KeepalivePing::PingType::REQUEST()) {
				$self->{'error'} = "IS2 ping: Wrong type of ping frame received: " . $ping->get_ping_type();
				return undef;
			}
			
			my $im = APRSIS2::IS2Message->new({
				'type' => APRSIS2::IS2Message::Type::KEEPALIVE_PING(),
				'keepalive_ping' => APRSIS2::IS2KeepalivePing->new({
					'ping_type' => APRSIS2::IS2KeepalivePing::PingType::REPLY(),
					'request_id' => $ping->get_request_id(),
					'request_data' => $ping->get_request_data()
				})
			});
			$self->is2_frame_out($im->encode);
			if (time() > $end_t) {
				warn "is2_frame_in_nonping: timeout\n";
				$self->{'error'} = "is2_frame_in_nonping timed out";
				return undef;
			}
		} else {
			return $l;
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

=head1 bind_and_listen()

Listen on an UDP peergroup socket

=cut

sub bind_and_listen($)
{
	my($self) = @_;
	
	my($localaddr, $localport) = split(':', $self->{'host_port'}); # TODO: ipv6...
	
	$self->{'usock'} = IO::Socket::INET->new(
		Proto => 'udp',
		LocalPort => $localport,
		LocalAddr => $localaddr,
		ReuseAddr => 1,
		ReeusePort => 1);
	
	if (!defined($self->{'usock'})) {
		$self->{'error'} = "Failed to bind an UDP client socket: $@";
		return 0;
        }
        
        $self->{'usock'}->sockopt(SO_RCVBUF, 32768);
        $self->{'usock'}->sockopt(SO_SNDBUF, 32768);
        
        warn "bound udp port $localaddr $localport, rcvbuf " .  $self->{'usock'}->sockopt(SO_RCVBUF)
        	 . " sndbuf " . $self->{'usock'}->sockopt(SO_SNDBUF) . "\n";
        
        $self->{'state'} = 'connected';
        return 1;
}

sub set_destination($$$)
{
        my($self, $hostport, $peer_call) = @_;
        
	my($host, $port) = split(':', $hostport); # TODO: ipv6...
	
	my $hisiaddr = inet_aton($host) || die "unknown host";
	my $hispaddr = sockaddr_in($port, $hisiaddr);
	
	$self->{'dest'} = $hispaddr;
	$self->{'peer_call'} = $hispaddr;
}

sub unbind($)
{
	my($self) = @_;
	
	undef $self->{'usock'};
}

sub send_udp_packet($$)
{
	my($self, $data) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	
	#warn "sending\n";
	my $ret = $self->{'usock'}->send($data, 0, $self->{'dest'});
	if (!$ret) {
		$self->{'error'} = "Failed to send an UDP packet: $!";
	}
	return $ret;
}

sub is2_corepeer_negotiate($)
{
        my($self) = @_;
        my $hello = "# PEER2 v 2 hello " . $self->{'mycall'} . " vers perl-is1 1.0 token hq736t7q63";
        if (!$self->send_udp_packet($hello)) {
        	return undef;
        }
	while (!$self->is2_corepeer_linked()) {
		my $packet = $self->is2_udp_frame_in(10);
	}
        return 1;
}

sub is2_corepeer_linked($)
{
	my($self) = @_;

	return (defined $self->{'corepeer-ok-tx'} && ($self->{'corepeer-ok-rx'}));
}

sub packet_is_equal($$$)
{
	my($self, $received, $expected) = @_;

	if (!ref($received)) {
		return $received eq $expected;
	}

	my $rec_pd = $received->get_is_packet_data();
	if (!defined $rec_pd) {
		return 0;
	}
	my $exp_pd = $expected->get_is_packet_data();
	if (!defined $exp_pd) {
		return 0;
	}

	my $exp_rx_rssi = $expected->get_rx_rssi();
	if (!defined $exp_rx_rssi) {
		return 0 if (defined $received->get_rx_rssi());
	} else {
		return 0 if ($exp_rx_rssi ne $received->get_rx_rssi());
	}
	
	my $exp_rx_snr_db = $expected->get_rx_snr_db();
	if (!defined $exp_rx_snr_db) {
		return 0 if (defined $received->get_rx_snr_db());
	} else {
		return 0 if ($exp_rx_snr_db ne $received->get_rx_snr_db());
	}
	
	return $rec_pd eq $exp_pd;
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

