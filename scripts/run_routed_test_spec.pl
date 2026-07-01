#!/usr/bin/perl

use strict;
use warnings;

use FindBin;
use lib $FindBin::Bin;
use Text::ParseWords qw(shellwords);

use CppgmBatchWorker qw(collect_tests);

sub usage
{
	die "Usage: run_routed_test_spec.pl <test-spec> <glob>[|<glob>]::<command with {tests}> ...\n";
}

sub glob_to_regex
{
	my ($glob) = @_;
	my $re = quotemeta($glob);
	$re =~ s/\\\*/.*/g;
	$re =~ s/\\\?/./g;
	return qr/^$re$/;
}

sub parse_route
{
	my ($text) = @_;
	my ($patterns, $command) = split(/::/, $text, 2);
	usage() if !defined($patterns) || !defined($command) || $patterns eq '' || $command eq '';
	my @regexes = map { glob_to_regex($_) } split(/\|/, $patterns);
	return { command => $command, regexes => \@regexes };
}

sub route_index_for_test
{
	my ($routes, $test) = @_;
	for (my $i = 0; $i < scalar(@{$routes}); ++$i)
	{
		for my $re (@{$routes->[$i]{regexes}})
		{
			return $i if $test =~ $re;
		}
	}
	return undef;
}

sub run_command_template
{
	my ($template, $tests) = @_;
	my @argv = shellwords($template);
	die "Empty routed check command\n" if scalar(@argv) == 0;
	my %env;
	while (@argv && $argv[0] =~ /^([A-Za-z_][A-Za-z0-9_]*)=(.*)$/)
	{
		$env{$1} = $2;
		shift @argv;
	}
	die "Empty routed check command after environment assignments\n" if scalar(@argv) == 0;
	my $test_spec = join(' ', @{$tests});
	for my $arg (@argv)
	{
		$arg =~ s/\{tests\}/$test_spec/g;
	}
	local %ENV = (%ENV, %env);
	system(@argv);
	return $? == 0 ? 0 : 1;
}

usage() if scalar(@ARGV) < 2;
my $test_spec = shift(@ARGV);
my @routes = map { parse_route($_) } @ARGV;
my @tests = collect_tests($test_spec, qr/\.t$/);
my @groups;
for my $test (@tests)
{
	my $index = route_index_for_test(\@routes, $test);
	die "No check route matches test '$test'\n" if !defined($index);
	push @{$groups[$index]}, $test;
}

my $failed = 0;
for (my $i = 0; $i < scalar(@routes); ++$i)
{
	next if !defined($groups[$i]) || scalar(@{$groups[$i]}) == 0;
	$failed = 1 if run_command_template($routes[$i]{command}, $groups[$i]) != 0;
}
exit($failed ? 1 : 0);
