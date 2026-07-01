#!/usr/bin/perl

use strict;
use warnings;

use Cwd qw(getcwd);
use File::Compare qw(compare);
use File::Basename qw(basename);
use File::Path qw(remove_tree);
use FindBin;
use lib $FindBin::Bin;
use Text::ParseWords qw(shellwords);

use CppgmBatchWorker qw(
	clear_progress_state
	close_worker
	collect_tests
	ensure_test_app_available
	get_timeout_from_env
	note_progress_state
	open_worker
	print_test_run_summary
	read_word_list
	resolve_host_command
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

sub host_target_name
{
	return 'macos' if $^O eq 'darwin';
	return 'linux' if $^O eq 'linux';
	return '';
}

sub host_target_triple_name
{
	return 'x86_64-apple-darwin' if $^O eq 'darwin';
	return 'x86_64-unknown-linux-gnu' if $^O eq 'linux';
	return '';
}

sub append_file
{
	my ($path, $contents) = @_;
	open(my $fh, '>>', $path) or die "Unable to append $path: $!";
	print $fh $contents;
	close($fh) or die "Unable to close $path: $!";
}

sub run_program
{
	my ($program, $test_base, $stdout_file, $stderr_file, $status_file) = @_;
	my $status = run_command_capture(
		cmd => [$program],
		stdout => $stdout_file,
		stderr => $stderr_file,
		stdin => (-f "$test_base.stdin" ? "$test_base.stdin" : undef),
		timeout => get_timeout_from_env("CPPGM_PROGRAM_TEST_TIMEOUT_SEC", 10),
	);
	write_numeric_status($status_file, $status);
}

if (scalar(@ARGV) != 3)
{
	die "Usage: run_cpptoolchain_tests_worker.pl <app> <suffix> <testlocation>";
}

sub process_one_test
{
	my ($suffix, $test, $worker_out, $worker_in) = @_;
	note_progress_state('prepare', $test);
	my $test_base = $test;
	$test_base =~ s/\.t$//;
	my $build_timeout = get_timeout_from_env("CPPGM_BUILD_TEST_TIMEOUT_SEC", 30);

	unlink(glob("$test_base.$suffix.program"));
	unlink(glob("$test_base.$suffix.direct.program"));
	unlink(glob("$test_base.$suffix.mixed.program"));
	unlink(glob("$test_base.$suffix.program.exit_status"));
	unlink(glob("$test_base.$suffix.direct.program.exit_status"));
	unlink(glob("$test_base.$suffix.mixed.program.exit_status"));
	unlink(glob("$test_base.$suffix.program.stdout"));
	unlink(glob("$test_base.$suffix.direct.program.stdout"));
	unlink(glob("$test_base.$suffix.mixed.program.stdout"));
	unlink(glob("$test_base.$suffix.program.stderr"));
	unlink(glob("$test_base.$suffix.direct.program.stderr"));
	unlink(glob("$test_base.$suffix.mixed.program.stderr"));
	unlink(glob("$test_base.$suffix.impl.stdout"));
	unlink(glob("$test_base.$suffix.impl.stderr"));
	unlink(glob("$test_base.$suffix.impl.exit_status"));
	unlink(glob("$test_base.$suffix.*.obj"));
	remove_tree("$test_base.$suffix.libdir", {error => \my $remove_error});

	my @srcfiles = sort glob("$test_base.t.*");
	if (scalar(@srcfiles) == 0)
	{
		write_numeric_status("$test_base.$suffix.impl.exit_status", 1);
		return;
	}

	my $impl_stdout = "$test_base.$suffix.impl.stdout";
	my $impl_stderr = "$test_base.$suffix.impl.stderr";
	write_file($impl_stdout, '');
	write_file($impl_stderr, '');

	my $host_cc_str =
		defined($ENV{CPPGM_HOST_CC}) && $ENV{CPPGM_HOST_CC} ne '' ? $ENV{CPPGM_HOST_CC} :
		defined($ENV{CC}) && $ENV{CC} ne '' ? $ENV{CC} :
		'';
	my $host_cxx_str =
		defined($ENV{CPPGM_HOST_CXX}) && $ENV{CPPGM_HOST_CXX} ne '' ? $ENV{CPPGM_HOST_CXX} :
		defined($ENV{CXX}) && $ENV{CXX} ne '' ? $ENV{CXX} :
		'';
	my @host_cc = resolve_host_command($host_cc_str, 'clang-22', 'clang', 'gcc', 'cc', 'c++');
	my @host_cxx = resolve_host_command($host_cxx_str,
	                                   'clang++-22',
	                                   'clang++',
	                                   'g++',
	                                   'c++');
	die "Unable to resolve host C compiler\n" if scalar(@host_cc) == 0;
	die "Unable to resolve host C++ compiler\n" if scalar(@host_cxx) == 0;

	my @flags = read_word_list("$test_base.flags");

	my $libdir = '';
	my $helper_status = 0;
	my @helper_sources =
		grep { $_ =~ m/\.(?:c|cc|cpp|cxx)$/ } glob("$test_base.lib.*");
	if (scalar(@helper_sources) != 0)
	{
		$libdir = "$test_base.$suffix.libdir";
		mkdir($libdir) or die "Unable to create $libdir: $!";
		for my $src (@helper_sources)
		{
			my $name = $src;
			$name =~ s/^\Q$test_base.lib.\E//;
			$name =~ s/\.[^.]+$//;
			my $obj = "$libdir/lib$name.o";
			my @compiler = $src =~ m/\.c$/ ? @host_cc : @host_cxx;
			$helper_status = run_command_capture(
				cmd => [@compiler, '-c', '-o', $obj, $src],
				stdout => $impl_stdout,
				stderr => $impl_stderr,
				timeout => $build_timeout,
			);
			last if $helper_status != 0;
		}
	}

	my $host_target = host_target_name();
	my $host_target_triple = host_target_triple_name();
	@flags = map {
		my $flag = $_;
		$flag =~ s/__LIBDIR__/$libdir/g if $libdir ne '';
		$flag =~ s/__HOST_TARGET__/$host_target/g if $host_target ne '';
		$flag =~ s/__HOST_TARGET_TRIPLE__/$host_target_triple/g if $host_target_triple ne '';
		$flag;
	} @flags;

	my @objfiles;
	my $compile_status = $helper_status;
	for my $src (@srcfiles)
	{
		note_progress_state('compile', $test);
		last if $compile_status != 0;
		my ($index) = ($src =~ m/\.([^.]+)$/);
		my $objfile = "$test_base.$suffix.$index.obj";
		push @objfiles, $objfile;
		$compile_status = submit_cli_request(
			$worker_in,
			$worker_out,
			$impl_stdout,
			$impl_stderr,
			{ CPPGM_BATCH_TIMEOUT_SEC => $build_timeout },
			@flags,
			'-c',
			'-o',
			$objfile,
			$src);
	}

	my $impl_status = $compile_status;
	my $direct_status = $compile_status;
	my $mixed_status = $compile_status;
	if ($compile_status == 0)
	{
		note_progress_state('link', $test);
		$impl_status = submit_cli_request(
			$worker_in,
			$worker_out,
			$impl_stdout,
			$impl_stderr,
			{ CPPGM_BATCH_TIMEOUT_SEC => $build_timeout },
			@flags,
			'-o',
			"$test_base.$suffix.program",
			@objfiles);
		note_progress_state('direct-link', $test);
		$direct_status = submit_cli_request(
			$worker_in,
			$worker_out,
			$impl_stdout,
			$impl_stderr,
			{ CPPGM_BATCH_TIMEOUT_SEC => $build_timeout },
			@flags,
			'-o',
			"$test_base.$suffix.direct.program",
			@srcfiles);
		if (scalar(@srcfiles) > 1)
		{
			note_progress_state('mixed-link', $test);
			my @mixed_args = (
				@flags,
				'-o',
				"$test_base.$suffix.mixed.program",
				$objfiles[0],
				@srcfiles[1..$#srcfiles],
			);
			$mixed_status = submit_cli_request(
				$worker_in,
				$worker_out,
				$impl_stdout,
				$impl_stderr,
				{ CPPGM_BATCH_TIMEOUT_SEC => $build_timeout },
				@mixed_args);
		}
	}

	write_numeric_status("$test_base.$suffix.impl.exit_status", $impl_status);

	if ($direct_status == 0)
	{
		note_progress_state('direct-run', $test);
		run_program(
			"$test_base.$suffix.direct.program",
			$test_base,
			"$test_base.$suffix.direct.program.stdout",
			"$test_base.$suffix.direct.program.stderr",
			"$test_base.$suffix.direct.program.exit_status");
	}

	if (scalar(@srcfiles) > 1 && $mixed_status == 0)
	{
		note_progress_state('mixed-run', $test);
		run_program(
			"$test_base.$suffix.mixed.program",
			$test_base,
			"$test_base.$suffix.mixed.program.stdout",
			"$test_base.$suffix.mixed.program.stderr",
			"$test_base.$suffix.mixed.program.exit_status");
	}

	if ($impl_status == 0)
	{
		note_progress_state('run', $test);
		run_program(
			"$test_base.$suffix.program",
			$test_base,
			"$test_base.$suffix.program.stdout",
			"$test_base.$suffix.program.stderr",
			"$test_base.$suffix.program.exit_status");
	}

	if ($impl_status != $direct_status)
	{
		append_file($impl_stderr,
		            "ERROR: direct source link status differs from separate compile/link\n");
		write_numeric_status("$test_base.$suffix.impl.exit_status", 1);
		unlink(glob("$test_base.$suffix.program"));
		unlink(glob("$test_base.$suffix.program.exit_status"));
		unlink(glob("$test_base.$suffix.program.stdout"));
		unlink(glob("$test_base.$suffix.program.stderr"));
	}
	elsif (scalar(@srcfiles) > 1 && $impl_status != $mixed_status)
	{
		append_file($impl_stderr,
		            "ERROR: mixed source/object link status differs from separate compile/link\n");
		write_numeric_status("$test_base.$suffix.impl.exit_status", 1);
		unlink(glob("$test_base.$suffix.program"));
		unlink(glob("$test_base.$suffix.program.exit_status"));
		unlink(glob("$test_base.$suffix.program.stdout"));
		unlink(glob("$test_base.$suffix.program.stderr"));
	}
	elsif ($impl_status == 0)
	{
		if (compare("$test_base.$suffix.program.stdout",
		            "$test_base.$suffix.direct.program.stdout") != 0 ||
		    compare("$test_base.$suffix.program.exit_status",
		            "$test_base.$suffix.direct.program.exit_status") != 0)
		{
			append_file($impl_stderr,
			            "ERROR: direct source link output differs from separate compile/link\n");
			write_numeric_status("$test_base.$suffix.impl.exit_status", 1);
			unlink(glob("$test_base.$suffix.program"));
			unlink(glob("$test_base.$suffix.program.exit_status"));
			unlink(glob("$test_base.$suffix.program.stdout"));
			unlink(glob("$test_base.$suffix.program.stderr"));
		}
		elsif (scalar(@srcfiles) > 1 &&
		       (compare("$test_base.$suffix.program.stdout",
		                "$test_base.$suffix.mixed.program.stdout") != 0 ||
		        compare("$test_base.$suffix.program.exit_status",
		                "$test_base.$suffix.mixed.program.exit_status") != 0))
		{
			append_file($impl_stderr,
			            "ERROR: mixed source/object link output differs from separate compile/link\n");
			write_numeric_status("$test_base.$suffix.impl.exit_status", 1);
			unlink(glob("$test_base.$suffix.program"));
			unlink(glob("$test_base.$suffix.program.exit_status"));
			unlink(glob("$test_base.$suffix.program.stdout"));
			unlink(glob("$test_base.$suffix.program.stderr"));
		}
	}

	unlink(@objfiles);
	unlink(glob("$libdir/lib*.o")) if $libdir ne '';
	unlink(glob("$test_base.$suffix.direct.program"));
	unlink(glob("$test_base.$suffix.direct.program.exit_status"));
	unlink(glob("$test_base.$suffix.direct.program.stdout"));
	unlink(glob("$test_base.$suffix.direct.program.stderr"));
	unlink(glob("$test_base.$suffix.mixed.program"));
	unlink(glob("$test_base.$suffix.mixed.program.exit_status"));
	unlink(glob("$test_base.$suffix.mixed.program.stdout"));
	unlink(glob("$test_base.$suffix.mixed.program.stderr"));
	unlink(glob("$test_base.$suffix.program"));
	unlink(glob("$test_base.$suffix.program.stderr"));
	unlink($impl_stdout, $impl_stderr);
	remove_tree($libdir) if $libdir ne '';
}

sub run_toolchain_tests
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
	run_toolchain_tests($app, $suffix, \@tests, $verbose);
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
		run_toolchain_tests($app, $suffix, $shard, $verbose);
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
