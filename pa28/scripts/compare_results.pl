#!/usr/bin/perl
use strict;
use warnings;
use FindBin;
use File::Basename qw(dirname);

my $repo_root = dirname(dirname($FindBin::Bin));

if (scalar(@ARGV) != 3)
{
	die "Usage: compare_results.pl <ref_suffix> <my_suffix> <testlocation>";
}

my ($ref_suffix, $my_suffix, $tests_root) = @ARGV;

if (-f $tests_root)
{
	my $mode = $tests_root =~ m{(?:^|/)structural/} ? "mir_structural_t" : "mir_t";
	my $status = system("perl",
	                    "$repo_root/scripts/compare_results_common.pl",
	                    $mode,
	                    $ref_suffix,
	                    $my_suffix,
	                    $tests_root);
	exit($status >> 8) if $status != 0;
	exit 0;
}

my @suites = (
	["mir_t", "$tests_root/strict"],
	["mir_structural_t", "$tests_root/structural"],
	["mir_t", "$tests_root/behavior"],
);
my $ran_suite = 0;

for my $suite (@suites)
{
	my ($mode, $path) = @{$suite};
	next if !-d $path;
	$ran_suite = 1;
	my $status = system("perl",
	                    "$repo_root/scripts/compare_results_common.pl",
	                    $mode,
	                    $ref_suffix,
	                    $my_suffix,
	                    $path);
	if ($status != 0)
	{
		exit($status >> 8);
	}
}

if (!$ran_suite)
{
	my $status = system("perl",
	                    "$repo_root/scripts/compare_results_common.pl",
	                    "mir_t",
	                    $ref_suffix,
	                    $my_suffix,
	                    $tests_root);
	exit($status >> 8) if $status != 0;
}

exit 0;
