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

exec("perl", "$repo_root/scripts/compare_results_common.pl", "mir_structural_t", @ARGV)
	or die "exec failed: $!";
