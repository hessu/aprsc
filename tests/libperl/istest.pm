
package istest;

=head1 NAME

istest - a perl module for doing APRS-IS network server tests.

=cut

use 5.006;
use strict;
use warnings;
use Data::Dumper;

sub txrx($$$$$)
{
	my($ok, $i_tx, $i_rx, $tx, $rx) = @_;
	
	my $sent = $i_tx->sendline($tx);
	
	if (!$sent) {
		&$ok($sent, 1, "Failed to send line to server: '$tx'");
		return;
	}
	
	my $received = $i_rx->getline_noncomment();
	
	if (!defined $received) {
		if ($i_rx->{'state'} eq 'connected') {
			&$ok(1, 0, "Did not receive packet from server (timeout): '$tx'");
		} else {
			&$ok(1, 0, "Server connection went down after sending: '$tx'");
		}
		return;
	}
	
	if ($received ne $rx) {
		&$ok($received, $rx, "Server returned wrong line");
	}
	
	&$ok(1, 1, "ok");
}

sub should_drop($$$$$)
{
	my($ok, $i_tx, $i_rx, $tx, $helper) = @_;
	
	my $drop_key = 'drop.' . int(rand(1000000));
	my $sent = $i_tx->sendline($tx . ' ' . $drop_key);
	
	if (!$sent) {
		&$ok($sent, 1, "Failed to send line to server: '$tx'");
		return;
	}
	
	my $helper_key = 'helper.' . int(rand(1000000));
	$sent = $i_tx->sendline($helper. ' ' . $helper_key);
	
	if (!$sent) {
		&$ok($sent, 1, "Failed to send helper line to server: '$tx'");
		return;
	}
	
	my $received = $i_rx->getline_noncomment();
	
	if (!defined $received) {
		if ($i_rx->{'state'} eq 'connected') {
			&$ok(1, 0, "Did not receive packet from server (timeout): '$tx'");
		} else {
			&$ok(1, 0, "Server connection went down after sending: '$tx'");
		}
		return;
	}
	
	if ($received =~ /$helper_key/) {
		&$ok(1, 1, "ok, received helper packet only");
		return;
	}
	
	if ($received =~ /$drop_key/) {
		&$ok($received, $helper, "Server forwarded packet it should have dropped");
	} else {
		&$ok($received, $helper, "Server returned completely unexpected packet");
	}
	
	# get one more
	$i_rx->getline_noncomment();
}


1;
