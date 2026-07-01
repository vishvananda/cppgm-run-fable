#!/usr/bin/perl

use strict;
use warnings;

use File::Basename qw(dirname);
use FindBin;
use lib $FindBin::Bin;

use CppgmBatchWorker qw(collect_tests);

sub getrawdata
{
	my ($file) = @_;
	open(my $fh, '<', $file) or die "Unable to read $file\n";
	local $/;
	my $data = <$fh>;
	close($fh);
	return '' if !defined($data);
	return $data;
}

sub putrawdata
{
	my ($file, $data) = @_;
	open(my $fh, '>', $file) or die "Unable to write $file\n";
	print $fh $data;
	close($fh);
}

sub canonicalize_machine_ir
{
	my ($path) = @_;
	my $common = dirname($FindBin::Bin) . "/scripts/compare_results_common.pl";
	open(my $fh, "-|", "perl", $common, "canonicalize-machine-ir", $path)
		or die "Unable to run machine IR canonicalizer\n";
	local $/;
	my $data = <$fh>;
	close($fh) or die "Machine IR canonicalizer failed for $path\n";
	return '' if !defined($data);
	return $data;
}

if (scalar(@ARGV) != 1)
{
	die "Usage: canonicalize_lowir_native_refs.pl <test-file-or-directory>\n";
}

my ($tests_root) = @ARGV;
my @tests = collect_tests($tests_root, qr/\.t$/);

for my $test (@tests)
{
	my $base = $test;
	$base =~ s/\.t$//;
	my $mir = "$base.ref.mir";
	my $cmir = "$base.ref.cmir";
	if (-f $mir)
	{
		putrawdata($cmir, canonicalize_machine_ir($mir));
	}
	else
	{
		unlink($cmir);
	}
}
