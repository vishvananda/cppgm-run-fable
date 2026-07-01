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
	run_command_capture
	submit_cli_request
	write_file
	write_numeric_status
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
	my ($suffix, $test, $worker_out, $worker_in) = @_;
	note_progress_state('prepare', $test);
	my $test_base = $test;
	$test_base =~ s/\.t$//;

	unlink(glob("$test_base.$suffix.map"));
	unlink(glob("$test_base.$suffix.program"));
	unlink(glob("$test_base.$suffix.program.exit_status"));
	unlink(glob("$test_base.$suffix.program.stdout"));
	unlink(glob("$test_base.$suffix.program.stderr"));
	unlink(glob("$test_base.$suffix.impl.stdout"));
	unlink(glob("$test_base.$suffix.impl.stderr"));
	unlink(glob("$test_base.$suffix.impl.exit_status"));
	unlink(glob("$test_base.$suffix.*.obj"));

	my @srcfiles = sort glob("$test_base.t.*");
	if (scalar(@srcfiles) == 0)
	{
		write_file("$test_base.$suffix.impl.stderr",
		           "ERROR: no source files for $test_base\n");
		write_numeric_status("$test_base.$suffix.impl.exit_status", 1);
		return;
	}

	my $impl_stdout = "$test_base.$suffix.impl.stdout";
	my $impl_stderr = "$test_base.$suffix.impl.stderr";
	write_file($impl_stdout, '');
	write_file($impl_stderr, '');

	my @objfiles;
	my $build_timeout = get_timeout_from_env("CPPGM_BUILD_TEST_TIMEOUT_SEC", 30);
	my $compile_status = 0;
	for my $src (@srcfiles)
	{
		note_progress_state('compile', $test);
		my ($index) = ($src =~ m/\.([^.]+)$/);
		my $objfile = "$test_base.$suffix.$index.obj";
		push @objfiles, $objfile;
		$compile_status = submit_cli_request($worker_in,
		                                     $worker_out,
		                                     $impl_stdout,
		                                     $impl_stderr,
		                                     { CPPGM_BATCH_TIMEOUT_SEC => $build_timeout },
		                                     '-c',
		                                     '-o',
		                                     $objfile,
		                                     $src);
		last if $compile_status != 0;
	}

	if ($compile_status != 0)
	{
		write_numeric_status("$test_base.$suffix.impl.exit_status", $compile_status);
		unlink(@objfiles);
		unlink(glob("$test_base.$suffix.map"));
		unlink(glob("$test_base.$suffix.program"));
		unlink(glob("$test_base.$suffix.program.exit_status"));
		unlink(glob("$test_base.$suffix.program.stdout"));
		unlink(glob("$test_base.$suffix.program.stderr"));
		unlink($impl_stdout, $impl_stderr);
		return;
	}

	note_progress_state('link', $test);
	my $impl_status = submit_cli_request($worker_in,
	                                     $worker_out,
	                                     $impl_stdout,
	                                     $impl_stderr,
	                                     { CPPGM_BATCH_TIMEOUT_SEC => $build_timeout },
	                                     '--dump-link-map',
	                                     "$test_base.$suffix.map",
	                                     '-o',
	                                     "$test_base.$suffix.program",
	                                     @objfiles);
	write_numeric_status("$test_base.$suffix.impl.exit_status", $impl_status);
	unlink(@objfiles);

	if ($impl_status == 0)
	{
		note_progress_state('run', $test);
		my $program_status = run_command_capture(
			cmd => ["$test_base.$suffix.program"],
			stdout => "$test_base.$suffix.program.stdout",
			stderr => "$test_base.$suffix.program.stderr",
			stdin => (-f "$test_base.stdin" ? "$test_base.stdin" : undef),
			timeout => get_timeout_from_env("CPPGM_PROGRAM_TEST_TIMEOUT_SEC", 10),
		);
		write_numeric_status("$test_base.$suffix.program.exit_status", $program_status);
		unlink("$test_base.$suffix.program", "$test_base.$suffix.program.stderr");
	}
	else
	{
		unlink(glob("$test_base.$suffix.map"));
		unlink(glob("$test_base.$suffix.program"));
		unlink(glob("$test_base.$suffix.program.exit_status"));
		unlink(glob("$test_base.$suffix.program.stdout"));
		unlink(glob("$test_base.$suffix.program.stderr"));
	}

	unlink($impl_stdout, $impl_stderr);
}

sub run_link_tests
{
	my ($app, $suffix, $tests, $verbose) = @_;
	my ($worker_pid, $worker_out, $worker_in) = open_worker($app);
	for my $test (@{$tests})
	{
		print "Running $test...\n" if $verbose;
		process_one_test($suffix, $test, $worker_out, $worker_in);
	}
	close_worker($worker_pid, $worker_out, $worker_in);
}

if (scalar(@ARGV) != 3)
{
	die "Usage: run_lowir_link_tests_worker.pl <app> <suffix> <testlocation>";
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
	run_link_tests($app, $suffix, \@tests, $verbose);
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
		run_link_tests($app, $suffix, $shard, $verbose);
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
