#!/usr/bin/perl

use strict;
use warnings;

use Config;
use Socket;
use LWP::UserAgent;

my $base = "/opt/aprsc";
my $acl_file = "$base/etc/t2.acl";
my $pid_file = "$base/logs/aprsc.pid";

my $url = "http://www.aprs2.net/t2servers.php";
# If generated ACL has less than N rows, something went wrong.
my $rows_min = 90;

sub isip6 {
	# no doubt, this could be done more cleanly and much faster
	if (($_[0] =~ /:::/) || ($_[0] !~ /^[0-9A-Fa-f:]+$/)) { return ""; }
	my(@l, $w, $s);
	$s = lc($_[0]);
	my $i = $s =~  s/:/:/g;
	my $repl = ':0' x (9 - $i);
	$repl =~ s/0$// if ($s !~ /:$/);
	$s =~ s/::/$repl/;
	$s =~ s/^:/0:/;
	
	#warn "isip6: $_[0] expanded to $s\n";
	@l = split(':', $s);
	if ($#l ne 7) {
		# want 8 words - bail out
		return "";
	}
	
	$s = "";
	foreach $w (@l) {
		if ($w =~ /[0-9a-f]{1-4}/) { return ""; }
		$repl = '0' x (4 - length($w));
		$s .= pack("H4", "$repl$w");
	}
	#warn "isip6: $_[0] => " . ipout6($s) . "\n";
	return $s;
}                                                                                                                                                                                                                                                        

my $ua = LWP::UserAgent->new(timeout => 5);
my $res = $ua->request(HTTP::Request->new('GET', $url));

if (!$res->is_success) {
	# todo: print result error messages
	die "Failed to download T2 ACL from $url\n";
}

my $new_acl = $res->content;

#warn "new: $new_acl\n";

my $acl;
my $rows = 0;
foreach my $a (split(/[\r\n]+/, $new_acl)) {
	$a =~ s/\s+//g;
	#warn "a: '$a'\n";
	
	# ipv4?
	if ($a =~ /^\d+\.\d+\.\d+\.\d+$/) {
		$acl .= "allow $a\n";
		$rows++;
		next;
	}
	
	# ipv6?
	if (isip6($a) ne "") {
		$acl .= "allow $a\n";
		$rows++;
		next;
	}
	
	if ($a =~ /^([A-Za-z][A-Za-z0-9-]*[A-Za-z0-9]\.)+[A-Za-z]{2,}$/) {
		#warn "lookup: $a\n";
		my $packed = gethostbyname($a);
		if (!defined $packed) {
			warn "T2 ACL: DNS lookup for $a failed\n";
			next;
		}
		
		my $ip = inet_ntoa($packed);
		#warn "lookup $a resolved to $ip\n";
		$acl .= "# resolved $a to $ip\nallow $ip\n";
		$rows++;
		next;
	}
	
	die "T2 ACL contains invalid address: $a\n";
}

#warn $acl;

if ($rows < $rows_min) {
	die "T2 ACL generated with less than $rows_min rows: $rows - suspecting failure, aborting\n";
}

my $acl_tmp = "$acl_file.tmp";
open(ACL, ">$acl_tmp") || die "Failed to open $acl_tmp for writing: $!\n";
print ACL $acl || die "Failed to write $acl_tmp: $!\n";
close(ACL) || die "Failed to close $acl_tmp after writing: $!\n";

rename($acl_tmp, $acl_file) || die "Failed to rename $acl_tmp to $acl_file: $!\n";

open(PID, $pid_file) || die "Failed to open $pid_file for reading: $!\n";
my $pid = <PID>;
close(PID) || die "Failed to close $pid_file after reading: $!\n";
chomp($pid);
if ($pid !~ /^\d+$/) {
	die "Did not find a numeric pid from $pid_file\n";
}

defined $Config{sig_name} || die "Config: No signals?";
my %signo;
my $i = 0;
foreach my $name (split(' ', $Config{sig_name})) {
	$signo{$name} = $i;
	$i++;
}

#warn "killing $pid with signal $signo{USR1}\n";
if (kill($signo{'USR1'}, $pid) != 1) {
	die "T2 ACL reload: Failed to reload aprsc <$pid> configuration with signal $signo{USR1}\n";
}
#warn "ok: " . join("\n", @acl) . "\n";
