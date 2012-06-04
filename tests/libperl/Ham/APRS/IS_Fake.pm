
package Ham::APRS::IS_Fake;

use 5.006;
use strict;
use warnings;

use IO::Handle '_IOFBF';
use IO::Socket::INET6;
use IO::Select;

use Ham::APRS::IS;

=head1 new(hostport, mycall, optionshash)

Initializes a new Ham::APRS::IS_Fake listening socket. Takes two mandatory arguments,
the host:port pair to listen on and the server's callsign.

  my $is = new Ham::APRS::IS_Fake('*:12765', 'N0CALL');

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
	
	my($localaddr, $localport) = split(':', $self->{'host_port'});
	
	$self->{'lsock'} = IO::Socket::INET6->new(
		Listen => 10,
		LocalAddr => $self->{'host_port'},
		Proto => 'tcp',
		ReuseAddr => 1,
		ReeusePort => 1);
	
	die "Could not create socket: $!\n" unless $self->{'lsock'};
}

sub unbind($)
{
	my($self) = @_;
	
	undef $self->{'lsock'};
}

sub accept($)
{
	my($self) = @_;
	
	my $sock = $self->{'lsock'}->accept();
	
	return if (!$sock);
	
	my $is = new Ham::APRS::IS('client:0', $self->{'mycall'});
	$is->accepted($sock);
	
	return $is;
}

## javAPRSSrvr 3.15b07
#user oh7lzb-af
## logresp oh7lzb-af unverified, server T2FINLAND

sub send_login_prompt($$)
{
	my($self, $is) = @_;
	
	return $is->sendline("# IS_Fake 1.00");
}

sub send_login_ok($$)
{
	my($self, $is) = @_;
	
	return $is->sendline("# logresp CALLSIGN unverified, server " . $self->{'mycall'} );
}

sub process_login($$)
{
	my($self, $is) = @_;
	
	my $r;
	
	$r = $is->sendline("# IS_Fake 1.00");
	if (!$r) {
		return 'failed to transmit server software string';
	}
	
	my $login = $is->getline_noncomment(1);

	if ($login !~ /^user\s+([^\s]+)\s+pass\s+(\d+)/) {
		return 'login command not understood: ' . $login;
	}
	
	$is->{'client_user'} = $1;
	my $pass_given = $2;
	
	my $passcode = Ham::APRS::IS::aprspass($1);
	
	my $verified = ($pass_given eq $passcode) ? 'verified' : 'unverified';
	
	$r = $is->sendline("# logresp " . $is->{'client_user'} . " $verified, server " . $self->{'mycall'} );
	if (!$r) {
		return 'failed to transmit logresp';
	}
	
	return 'ok';
}

1;
