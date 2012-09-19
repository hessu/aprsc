
package Ham::APRS::IS_Fake_UDP;

use 5.006;
use strict;
use warnings;

use IO::Handle '_IOFBF';
use IO::Socket::INET;
use IO::Select;

use Ham::APRS::IS;

=head1 new(hostport, mycall, optionshash)

Initializes a new Ham::APRS::IS_Fake_UDP listening socket. Takes two mandatory arguments,
the host:port pair to listen on and the server's callsign.

  my $udp = new Ham::APRS::IS_Fake_UDP('*:12765', 'N0CALL');

=cut

sub new($$$;%)
{
	my $that = shift;
	my $class = ref($that) || $that;
	my $self = { };
	bless ($self, $class);
	
	my($host_port, $mycall, %options) = @_;
	
	$self->{'host_port'} = $host_port;
	if (defined $mycall) {
		$self->{'mycall'} = $mycall;
	} else {
		$self->{'mycall'} = "FAKE" . sprintf("%d", rand(999));
	}
	#$self->{'filter'} = $options{'filter'} if (defined $options{'filter'});
	
	#warn "aprspass for $self->{mycall} is $self->{aprspass}\n";
	
	$self->{'state'} = 'init';
	$self->{'error'} = "No errors yet.";
	
	return $self;
}

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

sub set_destination($$)
{
        my($self, $hostport) = @_;
        
	my($host, $port) = split(':', $hostport); # TODO: ipv6...
	
	my $hisiaddr = inet_aton($host) || die "unknown host";
	my $hispaddr = sockaddr_in($port, $hisiaddr);
	
	$self->{'dest'} = $hispaddr;
}

sub unbind($)
{
	my($self) = @_;
	
	undef $self->{'usock'};
}

sub getline($;$)
{
	my($self, $timeout) = @_;
	
	return undef if ($self->{'state'} ne 'connected');
	
	$timeout = 5 if (!defined $timeout);
	
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

sub sendline($$)
{
	my($self, $line) = @_;
	
	#warn "udp sendline: $line\n";
	
	return undef if ($self->{'state'} ne 'connected');
	
	#warn "sending\n";
	$self->{'usock'}->send($line, 0, $self->{'dest'});
	
	#warn "sent, returning 1\n";
	return 1;
}


1;
