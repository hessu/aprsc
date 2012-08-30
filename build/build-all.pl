#!/usr/bin/perl

use strict;
use warnings;

use Time::HiRes qw(time sleep);
use IO::Socket::INET;

my @platforms = (
	'ubuntu-1204-i386',
	'ubuntu-1204-amd64',
	'ubuntu-1004-i386',
	'ubuntu-1004-amd64',
	'debian-60-i386',
	'debian-60-amd64',
);

my($tgz, $buildonly) = @ARGV;

if (defined $buildonly) {
	@platforms = grep /$buildonly/, @platforms;
}

my $virsh = "virsh -c qemu:///system";

# temporary download directory
my $dir_build_down = "./build-down";
# final downloaded builds
my $dir_build_out = "./build-out";

system "mkdir -p $dir_build_out";

sub tcp_wait($$)
{
	my($hostport, $seconds) = @_;
	
	my $ok = 0;
	my $end = time() + $seconds;
	while (time() < $end) {
		my $sock = IO::Socket::INET->new($hostport);
		if (defined $sock) {
			# OK
			close($sock);
			warn " ... $hostport: ok\n";
			$ok++;
			return 1 if ($ok >= 2);
		} else {
			warn " ... $hostport: $!\n";
		}
		
		sleep(0.3);
	}
	
	return 0;
}

sub vm_state($)
{
	my($vm) = @_;
	
	my $state = `$virsh domstate $vm`;
	
	$state =~ s/\s+$//s;
	
	print "vm $vm: $state\n";
	
	return $state;
}

sub vm_up($)
{
	my($vm) = @_;
	
	my $state = vm_state($vm);
	
	if ($state eq "shut off") {
		system "$virsh start $vm";
		return 1;
	}
	
	if ($state eq "running") {
		print "vm_up $vm: vm already running\n";
		return 1;
	}
	
	die "vm_up $vm: unknown state $state\n";
}

sub vm_build($$$)
{
	my($vm, $distr, $tgz) = @_;
	
	sleep(2);
	
	tcp_wait("$vm:22", 15) || die "vm $vm ssh is not accepting connections\n";
	
	my $d_tgz = $tgz;
	$d_tgz =~ s/.*\///;
	my $dir = $d_tgz;
	$dir =~ s/\.tar.*//;
	
	print "... cleanup ...\n";
	system("ssh $vm 'rm -rf buildtmp && mkdir -p buildtmp'") == 0 or die "vm buildtmp cleanup failed: $?\n";
	print "... upload sources ...\n";
	system("scp $tgz $vm:buildtmp/") == 0 or die "vm source upload failed: $?\n";
	print "... extract sources ...\n";
	system("ssh $vm 'cd buildtmp && tar xvfz $d_tgz'") == 0 or die "vm source extract failed: $?\n";
	print "... build ...\n";
	system("ssh $vm 'cd buildtmp/$dir/src && ./configure && make make-deb'") == 0 or die "vm build phase failed: $?\n";
	print "... download packages ...\n";
	system("rm -rf $dir_build_down") == 0 or die "failed to delete $dir_build_down directory\n";
	mkdir($dir_build_down) || die "Could not mkdir $dir_build_down: $!\n";
	system("scp $vm:buildtmp/$dir/*.deb $vm:buildtmp/$dir/*.changes $dir_build_down/") == 0 or die "vm build product download failed: $?\n";
	
	opendir(my $dh, $dir_build_down) || die "Could not opendir $dir_build_down: $!\n";
	my @products = grep { /^aprsc.*\.(changes|deb)/ && -f "$dir_build_down/$_" } readdir($dh);
	closedir($dh);
	
	my $dist = $distr;
	$dist =~ s/-[^\-]+$//;
	
	foreach my $f (@products) {
		my $of = "$dir_build_out/$dist/$f";
		mkdir("$dir_build_out/$dist");
		rename("$dir_build_down/$f", $of) || die "Failed to rename $f to $of: $!\n";;
	}
	
	
}

sub vm_shutdown($)
{
	my($vm) = @_;
	
	my $state = vm_state($vm);
	
	if ($state eq "running") {
		system "$virsh shutdown $vm";
		return 1;
	}
	
	if ($state eq "shut off") {
		return 1;
	}
	
	die "vm_down $vm: unknown state $state\n";
}

sub build($$)
{
	my($plat, $tgz) = @_;
	
	print "Building $plat:\n";
	
	my $vm = "build-$plat";
	
	vm_up($vm);
	vm_build($vm, $plat, $tgz);
	vm_shutdown($vm);
}

# main

foreach my $plat (@platforms) {
	build($plat, $tgz);
}

