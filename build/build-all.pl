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
	'centos-63-i686',
	'centos-63-x86_64',
);

# temporary download directory
my $dir_build_down = "./build-down";
# final downloaded builds
my $dir_build_out = "./build-out";
# where to upload builds
my $upload_host = 'aprsc-dist';
# directory on upload host
my $dir_upload_tmp = '/data/www/aprsc-dist/tmp-upload';
# repository root directory
my $dir_upload_repo = '/data/www/aprsc-dist/html/aprsc/apt';

my $debug = 0;
my $virsh = "virsh -c qemu:///system";



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
			return 1 if ($ok >= 3);
		} else {
			warn " ... $hostport: $!\n";
		}
		
		sleep(0.3);
	}
	
	return if ($ok);
	
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
	
	tcp_wait("$vm:22", 30) || die "vm $vm ssh is not accepting connections\n";
	
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
		my $of = "$dir_build_out/$f";
#		mkdir("$dir_build_out/$dist");
		rename("$dir_build_down/$f", $of) || die "Failed to rename $f to $of: $!\n";;
	}
	
	
}

sub vm_build_rpm($$$)
{
	my($vm, $distr, $tgz) = @_;
	
	sleep(2);
	
	tcp_wait("$vm:22", 30) || die "vm $vm ssh is not accepting connections\n";
	
	my $d_tgz = $tgz;
	$d_tgz =~ s/.*\///;
	my $dir = $d_tgz;
	$dir =~ s/\.tar.*//;
	
	my $arch = $distr;
	$arch =~ s/.*-//;
	
	print "... cleanup ...\n";
	system("ssh $vm 'rm -rf buildtmp && mkdir -p buildtmp && rm -rf rpmbuild/*'") == 0 or die "vm buildtmp cleanup failed: $?\n";
	print "... upload sources ...\n";
	system("scp $tgz $vm:buildtmp/") == 0 or die "vm source upload failed: $?\n";
	print "... build RPM ...\n";
	system("ssh $vm 'cd buildtmp && rpmbuild --target $arch -ta $d_tgz'") == 0 or die "vm build phase failed: $?\n";
	print "... download packages ...\n";
	system("rm -rf $dir_build_down") == 0 or die "failed to delete $dir_build_down directory\n";
	mkdir($dir_build_down) || die "Could not mkdir $dir_build_down: $!\n";
	system("scp $vm:rpmbuild/RPMS/*/aprsc-*.rpm $dir_build_down/") == 0 or die "vm build product download failed: $?\n";
	
	opendir(my $dh, $dir_build_down) || die "Could not opendir $dir_build_down: $!\n";
	my @products = grep { /^aprsc-*\.(rpm)/ && -f "$dir_build_down/$_" } readdir($dh);
	closedir($dh);
	
	my $dist = $distr;
	$dist =~ s/-[^\-]+$//;
	
	foreach my $f (@products) {
		my $of = "$dir_build_out/$f";
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
	
	my $vm = "build-$plat";
	
	$vm =~ s/i686/i386/;
	$vm =~ s/x86_64/amd64/;
	
	print "Building $plat on $vm:\n";
	
	vm_up($vm);
	if ($plat =~ /centos/) {
		vm_build_rpm($vm, $plat, $tgz);
	} else {
		vm_build($vm, $plat, $tgz);
	}
	vm_shutdown($vm);
}

# main

my $help = "build-all.pl [-d] [-h] [-o <buildonly>] [-m <build|upload>] <source.tar.gz>\n";
my @args = @ARGV;
my $mode = 'build';
my $buildonly;
my @args_left;
while (my $par = shift @args) {
	if ($par eq "-d") { $debug = 1; print "Debugging...\n"; }
	elsif ($par eq "-h") { print $help; exit(0); }
	elsif ($par eq "-m") { $mode = shift @args; }
	elsif ($par eq "-o") { $buildonly = shift @args; }
	elsif ($par eq "-?") { print $help; exit(0); }
	else {
		if ($par =~ /^-/) {
			print "Unknown parameter \"$par\"\n$help"; exit(1);
		}
		push @args_left, $par;
	}
}

if ($mode eq 'build') {
	if ($#args_left != 0) {
		print $help;
		exit(1);
	}
	
	my($tgz) = @args_left;
	
	if (! -f $tgz) {
		warn "No such source package: $tgz\n";
		print $help;
		exit(1);
	}
	
	if (defined $buildonly) {
		@platforms = grep /$buildonly/, @platforms;
	}
	
	system "mkdir -p $dir_build_out";
	
	foreach my $plat (@platforms) {
		build($plat, $tgz);
	}
}

if ($mode eq 'upload') {
	system("ssh $upload_host 'rm -rf $dir_upload_tmp && mkdir -p $dir_upload_tmp'") == 0 or die "upload host upload_tmp cleanup failed: $?\n";
	system("scp -r $dir_build_out/* $upload_host:$dir_upload_tmp/") == 0 or die "upload to upload_host failed: $?\n";
	system("ssh -t $upload_host 'cd $dir_upload_repo && eval `gpg-agent --daemon`; for i in $dir_upload_tmp/*.changes;"
		. " do DIST=`echo \$i | perl -pe \"s/.*\\+([[:alpha:]]+).*/\\\\\$1/\"`;"
		. " echo dist \$DIST: \$i;"
		. " reprepro --ask-passphrase -Vb . include \$DIST \$i;"
		. " done; killall gpg-agent'") == 0
			or die "repository update failed: $?\n";
}

