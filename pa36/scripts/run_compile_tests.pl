#!/usr/bin/perl

use strict;
use warnings;
use File::Find;

sub detect_jobs
{
	my $jobs = $ENV{CPPGM_TEST_JOBS};
	if (defined($jobs) && $jobs =~ m/^\d+$/ && $jobs > 0)
	{
		return $jobs;
	}
	chomp($jobs = `getconf _NPROCESSORS_ONLN 2>/dev/null`);
	if ($jobs =~ m/^\d+$/ && $jobs > 0)
	{
		return $jobs;
	}
	return 1;
}

sub collect_tests
{
	my ($root, $pattern) = @_;
	die "Test path '$root' does not exist\n" if !-e $root;
	if (-f $root)
	{
		return $root =~ $pattern ? ($root) : ();
	}

	my @tests;
	find(sub {
		return if !-f $_;
		push @tests, $File::Find::name if $File::Find::name =~ $pattern;
	}, $root);
	return sort @tests;
}

sub write_named_status_code
{
	my ($path, $status) = @_;
	open(my $fh, '>', $path) or die "Unable to write $path: $!";
	if (!defined($status))
	{
		print $fh "EXIT_FAILURE\n";
	}
	elsif ($status == 0)
	{
		print $fh "EXIT_SUCCESS\n";
	}
	elsif ($status == 124)
	{
		print $fh "EXIT_TIMEOUT\n";
	}
	elsif ($status == 86)
	{
		print $fh "EXIT_NOT_IMPLEMENTED\n";
	}
	else
	{
		print $fh "EXIT_FAILURE\n";
	}
	close($fh) or die "Unable to close $path: $!";
}

sub system_status_to_exit_code
{
	my ($status) = @_;
	return 1 if !defined($status) || $status == -1;
	return $status >> 8 if ($status & 127) == 0;
	return 128 + ($status & 127);
}

sub ensure_test_app_available
{
	my ($app, $suffix, $tests) = @_;
	my $exec_path = $app =~ m{/} ? $app : "./$app";
	return if -x $exec_path;

	my $kind = $suffix eq 'ref' ? 'reference test app' : 'test app';
	my $message = "ERROR: $kind '$app' does not exist or is not executable";
	$message .= " at '$exec_path'" if $exec_path ne $app;
	$message .= ".\n";

	if ($suffix eq 'ref')
	{
		my $regular_app = $app;
		$regular_app =~ s/-ref$//;
		$message .= "No .ref files were written.\n";
		$message .= "Build/export the reference binary, or intentionally regenerate with the current compiler using:\n";
		$message .= "  make ref-test TEST=$tests REF_TEST_APP=$regular_app\n";
	}

	die $message;
}

sub run_tests
{
	my ($tests, $jobs, $runner) = @_;
	if ($jobs <= 1)
	{
		for my $test (@{$tests})
		{
			print "Running $test...\n";
			$runner->($test);
		}
		return;
	}

	my %children;
	for my $test (@{$tests})
	{
		print "Running $test...\n";
		while (scalar(keys %children) >= $jobs)
		{
			my $pid = waitpid(-1, 0);
			die "waitpid failed: $!" if $pid == -1;
			delete $children{$pid};
		}

		my $pid = fork();
		die "fork failed: $!" unless defined($pid);
		if ($pid == 0)
		{
			$runner->($test);
			exit 0;
		}
		$children{$pid} = 1;
	}

	while (%children)
	{
		my $pid = waitpid(-1, 0);
		die "waitpid failed: $!" if $pid == -1;
		delete $children{$pid};
	}
}

if (scalar(@ARGV) != 3)
{
	die "Usage: run_compile_tests.pl <app> <suffix> <testlocation>";
}

my $app = $ARGV[0];
my $suffix = $ARGV[1];
my $tests = $ARGV[2];
my $jobs = detect_jobs();

ensure_test_app_available($app, $suffix, $tests);
my @tests = collect_tests($tests, qr/\.t$/);
run_tests(\@tests, $jobs, sub {
	my ($test) = @_;
	my $test_out = $test;
	$test_out =~ s/\.t$/\.$suffix/;
	my $sys_ret = system("bash", "scripts/run_one_compile_test.sh", $app, $test, $test_out);
	write_named_status_code("$test_out.exit_status", system_status_to_exit_code($sys_ret));
});
