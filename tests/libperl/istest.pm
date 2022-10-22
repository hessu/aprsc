
package istest;

=head1 NAME

istest - a perl module for doing APRS-IS network server tests.

=cut

use 5.006;
use strict;
use warnings;
use Data::Dumper;

my $debug = 0;

sub txrx($$$$$)
{
	my($ok, $i_tx, $i_rx, $tx, $rx) = @_;
	
	warn "sending: $tx\n" if ($debug);
	my $sent = $i_tx->sendline($tx);
	
	warn "sent\n" if ($debug);
	
	if (!$sent) {
		&$ok(0, 1, "Failed to send line to server: '$tx'");
		return;
	}
	
	if ($i_tx->{'state'} ne 'connected') {
		&$ok(1, 0, "Server TX connection error after sending: '$tx': " . $i_tx->{'error'});
	}
	
	warn "receiving\n" if ($debug);
	
	my $received = $i_rx->getline_noncomment();
	
	if (!defined $received) {
		if ($i_rx->{'state'} eq 'connected') {
			&$ok(1, 0, "Did not receive packet from server (timeout): '$tx'");
		} else {
			&$ok(1, 0, "Server RX connection error after sending: '$tx': " . $i_rx->{'error'});
		}
		return;
	}
	
	warn "received '$rx'\n" if ($debug);
	
	if ($received ne $rx) {
		&$ok($received, $rx, "Server returned wrong line");
		return;
	}
	
	&$ok(1, 1, "ok");
}

sub should_drop($$$$$;$$)
{
	my($ok, $i_tx, $i_rx, $tx, $helper, $no_random_drop, $no_random_helper) = @_;
	
	my $drop_key = '';
	$drop_key .= ' drop.' . int(rand(1000000)) if (!$no_random_drop);
	my $tx_drop = $tx . $drop_key;
	warn "sending for drop: $tx_drop\n" if ($debug);
	my $sent = $i_tx->sendline($tx_drop);
	
	if (!$sent) {
		&$ok($sent, 1, "Failed to send line to server: '$tx'");
		return;
	}
	
	my $helper_key = 'helper.' . int(rand(1000000));
	my $helper_l = $helper;
	$helper_l .= ' ' . $helper_key if (!$no_random_helper);
	warn "sending for pass: $helper_l\n" if ($debug);
	$sent = $i_tx->sendline($helper_l);
	
	if (!$sent) {
		&$ok($sent, 1, "Failed to send helper line to server: '$helper_l'");
		return;
	}
	
	my $received = $i_rx->getline_noncomment();
	
	if (!defined $received) {
		if ($i_rx->{'state'} eq 'connected') {
			&$ok(1, 0, "Did not receive helper packet from server (timeout): '$helper_l'");
		} else {
			&$ok(1, 0, "Server connection went down after sending: '$tx' and '$helper_l'");
		}
		return;
	}
	warn "received: $received\n" if ($debug);
	
	my $tx2 = $tx;
	$tx2 =~ s/([^>]+)>([^,]+),[^:]+(:.*)$/$1>$2$3/;
	my $hl2 = $helper;
	$hl2 =~ s/([^>]+)>([^,]+),[^:]+(:.*)$/$1>$2$3/;
	my $rec2 = $received;
	$rec2 =~ s/([^>]+)>([^,]+),[^:]+(:.*)$/$1>$2$3/;
	
	if ($no_random_helper) {
		#warn "exp '$hl2'\n";
		#warn "got '$rec2'\n";
		if ($hl2 eq $rec2) {
			&$ok(1, 1, "ok, received helper packet only");
			return;
		}
	} else {
		if ($received =~ /$helper_key/) {
			&$ok(1, 1, "ok, received helper packet only");
			return;
		}
	}
	
	if ($no_random_drop) {
		if ($hl2 eq $tx2) {
			&$ok($received, $helper, "Server forwarded packet it should have dropped");
		} else {
			&$ok($received, $helper, "Server returned completely unexpected packet");
		}
	} else {
		if ($received =~ /$drop_key/) {
			&$ok($received, $helper, "Server forwarded packet it should have dropped");
		} else {
			&$ok($received, $helper, "Server returned completely unexpected packet");
		}
	}
	
	# since we received an extra packet, get one more line to receive the helper
	$i_rx->getline_noncomment();
}

sub read_and_disconnect($)
{
	my($i) = @_;
	
	
}

1;
