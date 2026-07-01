#!/usr/bin/perl

use strict;
use warnings;

use Cwd qw(getcwd);
use File::Basename qw(basename);
use FindBin;
use lib $FindBin::Bin;

use CppgmBatchWorker qw(
	clear_progress_state
	close_worker
	collect_tests
	ensure_test_app_available
	get_timeout_from_env
	note_progress_state
	open_worker
	print_test_run_summary
	read_env_file
	run_command_capture
	submit_cli_request
	write_named_status_code
);

sub detect_jobs
{
	my $jobs = $ENV{CPPGM_TEST_JOBS};
	return 1 if !defined($jobs);
	return 1 if $jobs !~ m/^\d+$/;
	return $jobs > 0 ? $jobs : 1;
}

sub process_one_test
{
	my ($app, $suffix, $test, $worker_out, $worker_in) = @_;
	note_progress_state('preproc', $test);
	my $test_base = $test;
	$test_base =~ s/\.t$//;
	my $test_out = $test;
	$test_out =~ s/\.t$/\.$suffix/;

	unlink($test_out, "$test_out.stdout", "$test_out.exit_status");

	my $env = read_env_file("$test_base.env");
	my @inputs = sort glob("$test*");
	my $status;
	my $build_timeout = get_timeout_from_env("CPPGM_BUILD_TEST_TIMEOUT_SEC", 30);
	my %worker_env = %{$env};
	$worker_env{CPPGM_BATCH_TIMEOUT_SEC} = $build_timeout;
	if (scalar(keys %{$env}) != 0)
	{
		$status = run_command_capture(
			cmd => [$app, '-E', '-o', $test_out, @inputs],
			stdout => "$test_out.stdout",
			stderr => "$test_out.stdout",
			env => \%worker_env,
			timeout => $build_timeout,
		);
	}
	else
	{
		$status = submit_cli_request(
			$worker_in,
			$worker_out,
			"$test_out.stdout",
			"$test_out.stdout",
			\%worker_env,
			'-E',
			'-o',
			$test_out,
			@inputs);
	}
	write_named_status_code("$test_out.exit_status", $status);
}

sub run_preproc_tests
{
	my ($app, $suffix, $tests, $verbose) = @_;
	my ($worker_pid, $worker_out, $worker_in) = open_worker($app);
	for my $test (@{$tests})
	{
		print "Running $test...\n" if $verbose;
		process_one_test($app, $suffix, $test, $worker_out, $worker_in);
	}
	close_worker($worker_pid, $worker_out, $worker_in);
}

if (scalar(@ARGV) != 3)
{
	die "Usage: run_cpphostcompat_preproc_worker.pl <app> <suffix> <testlocation>";
}

my ($app, $suffix, $tests_root) = @ARGV;
ensure_test_app_available($app, $suffix, $tests_root);
my @tests = collect_tests($tests_root, qr/\.t$/);
my $verbose = $ENV{VERBOSE} || $ENV{CPGM_TEST_VERBOSE};
my $keep_going = $ENV{KEEP_GOING};
my $assignment = basename(getcwd());
if (!$verbose && !$keep_going)
{
	print_test_run_summary($assignment, $tests_root, \@tests);
}
my $ntests = scalar(@tests);
my $jobs = detect_jobs();
$jobs = $ntests if $jobs > $ntests;
if ($jobs <= 1)
{
	clear_progress_state();
	run_preproc_tests($app, $suffix, \@tests, $verbose);
	clear_progress_state();
	exit 0;
}

clear_progress_state();
my @shards;
for (my $i = 0; $i < $jobs; ++$i)
{
	$shards[$i] = [];
}
for (my $i = 0; $i < @tests; ++$i)
{
	push @{$shards[$i % $jobs]}, $tests[$i];
}

my @pids;
for my $shard (@shards)
{
	next if scalar(@{$shard}) == 0;
	my $pid = fork();
	die "fork failed: $!" if !defined($pid);
	if ($pid == 0)
	{
		run_preproc_tests($app, $suffix, $shard, $verbose);
		exit 0;
	}
	push @pids, $pid;
}

my $failed = 0;
for my $pid (@pids)
{
	waitpid($pid, 0);
	$failed = 1 if $? != 0;
}

clear_progress_state();
exit($failed ? 1 : 0);
