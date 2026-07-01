#!/usr/bin/perl

use strict;
use warnings;

use Cwd qw(getcwd);
use File::Basename qw(basename);
use File::Path qw(remove_tree);
use FindBin;
use Text::ParseWords qw(shellwords);
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

sub shell_quote
{
	my ($s) = @_;
	return "''" if !defined($s) || $s eq '';
	$s =~ s/'/'"'"'/g;
	return "'$s'";
}

sub remove_previous_outputs
{
	my ($test_base, $suffix) = @_;
	unlink(glob("$test_base.$suffix.program"));
	unlink(glob("$test_base.$suffix.program.exit_status"));
	unlink(glob("$test_base.$suffix.program.stdout"));
	unlink(glob("$test_base.$suffix.program.stderr"));
	unlink(glob("$test_base.$suffix.inspect"));
	unlink(glob("$test_base.$suffix.impl.stdout"));
	unlink(glob("$test_base.$suffix.impl.stderr"));
	unlink(glob("$test_base.$suffix.impl.exit_status"));
	unlink(glob("$test_base.$suffix.impl.link_command"));
	unlink(glob("$test_base.$suffix.impl.link_verbose"));
	unlink(glob("$test_base.$suffix.impl.unresolved_symbols"));
	unlink(glob("$test_base.$suffix.*.o"));
	remove_tree("$test_base.$suffix.libdir", {error => \my $remove_error});
}

sub host_platform_tag
{
	chomp(my $os = `uname -s 2>/dev/null`);
	$os = lc($os);
	return 'linux' if $os eq 'linux';
	return $os;
}

sub read_text_file
{
	my ($path) = @_;
	open(my $fh, '<', $path) or die "Unable to read $path: $!";
	local $/;
	my $data = <$fh>;
	close($fh) or die "Unable to close $path: $!";
	$data = '' if !defined($data);
	return $data;
}

sub ensure_test_tool_path
{
	my ($env) = @_;
	my @rg = resolve_host_command('', 'rg');
	return if scalar(@rg) != 0;
	my $tool_dir = "$FindBin::Bin/test_shell_tools";
	my $path = exists($env->{PATH}) && defined($env->{PATH}) && $env->{PATH} ne ''
		? $env->{PATH}
		: ($ENV{PATH} || '');
	$env->{PATH} = $tool_dir;
	$env->{PATH} .= ":$path" if $path ne '';
}

sub replace_all_literal
{
	my ($text, $needle, $replacement) = @_;
	return $text if !defined($needle) || $needle eq '';
	$replacement = '' if !defined($replacement);
	my $offset = 0;
	while (1)
	{
		my $index = index($text, $needle, $offset);
		last if $index < 0;
		substr($text, $index, length($needle), $replacement);
		$offset = $index + length($replacement);
	}
	return $text;
}

if (scalar(@ARGV) != 3)
{
	die "Usage: run_cpphostinterop_tests_worker.pl <app> <suffix> <testlocation>";
}

sub impl_status_file
{
	my ($ctx) = @_;
	return "$ctx->{test_base}.$ctx->{suffix}.impl.exit_status";
}

sub build_test_context
{
	my ($suffix, $test, $host_tag) = @_;
	my $test_base = $test;
	$test_base =~ s/\.t$//;
	my $build_timeout = get_timeout_from_env("CPPGM_BUILD_TEST_TIMEOUT_SEC", 30);
	my $env = read_env_file("$test_base.env");
	my %worker_env = %ENV;
	for my $key (keys %{$env})
	{
		$worker_env{$key} = $env->{$key};
	}
	ensure_test_tool_path(\%worker_env);
	if (defined($env->{CPPGM_BUILD_TEST_TIMEOUT_SEC}) &&
		$env->{CPPGM_BUILD_TEST_TIMEOUT_SEC} =~ m/^\d+$/ &&
		$env->{CPPGM_BUILD_TEST_TIMEOUT_SEC} > 0)
	{
		$build_timeout = 0 + $env->{CPPGM_BUILD_TEST_TIMEOUT_SEC};
	}
	$worker_env{CPPGM_BATCH_TIMEOUT_SEC} = $build_timeout;

	my @srcfiles =
		sort grep { $_ =~ m/^\Q$test_base.t.\E\d+$/ } glob("$test_base.t.*");

	my $host_cxx_str =
		defined($env->{CPPGM_HOST_CXX}) && $env->{CPPGM_HOST_CXX} ne '' ? $env->{CPPGM_HOST_CXX} :
		defined($ENV{CPPGM_HOST_CXX}) && $ENV{CPPGM_HOST_CXX} ne '' ? $ENV{CPPGM_HOST_CXX} :
		defined($env->{CXX}) && $env->{CXX} ne '' ? $env->{CXX} :
		defined($ENV{CXX}) && $ENV{CXX} ne '' ? $ENV{CXX} :
		'';
	my $host_cc_str =
		defined($env->{CPPGM_HOST_CC}) && $env->{CPPGM_HOST_CC} ne '' ? $env->{CPPGM_HOST_CC} :
		defined($ENV{CPPGM_HOST_CC}) && $ENV{CPPGM_HOST_CC} ne '' ? $ENV{CPPGM_HOST_CC} :
		defined($env->{CC}) && $env->{CC} ne '' ? $env->{CC} :
		defined($ENV{CC}) && $ENV{CC} ne '' ? $ENV{CC} :
		'';
	my @host_cxx = resolve_host_command($host_cxx_str,
	                                   'clang++-22',
	                                   'clang++',
	                                   'g++',
	                                   'c++');
	my @host_cc = resolve_host_command($host_cc_str, 'clang-22', 'clang', 'gcc', 'cc', 'c++');
	die "Unable to resolve host C++ compiler\n" if scalar(@host_cxx) == 0;
	die "Unable to resolve host C compiler\n" if scalar(@host_cc) == 0;
	my @stdlib_flags = shellwords($ENV{CPPGM_STDLIB_FLAGS} || '');
	my @link_driver = @host_cxx;
	my @link_driver_override = read_word_list("$test_base.link.driver");
	@link_driver = @link_driver_override if scalar(@link_driver_override) != 0;
	my $shared_ext = 'so';

	my $impl_stdout = "$test_base.$suffix.impl.stdout";
	my $impl_stderr = "$test_base.$suffix.impl.stderr";
	my $link_command = "$test_base.$suffix.impl.link_command";
	my $link_verbose = "$test_base.$suffix.impl.link_verbose";
	my $nm_report = "$test_base.$suffix.impl.unresolved_symbols";
	write_file($impl_stdout, '');
	write_file($impl_stderr, '');

	my @compile_flags = read_word_list("$test_base.compile.flags");
	my @link_flags = read_word_list("$test_base.link.flags");
	my @run_args = read_word_list("$test_base.argv");
	my $inspect_cmd_file = -f "$test_base.inspect.cmd.$host_tag"
		? "$test_base.inspect.cmd.$host_tag"
		: (-f "$test_base.inspect.cmd" ? "$test_base.inspect.cmd" : '');
	my $inspect_expect_file = -f "$test_base.inspect.expect.$host_tag"
		? "$test_base.inspect.expect.$host_tag"
		: (-f "$test_base.inspect.expect" ? "$test_base.inspect.expect" : '');
	my $inspect_facts_file = -f "$test_base.inspect.facts.$host_tag"
		? "$test_base.inspect.facts.$host_tag"
		: (-f "$test_base.inspect.facts" ? "$test_base.inspect.facts" : '');
	my $inspect_plan_file = -f "$test_base.inspect.plan.$host_tag"
		? "$test_base.inspect.plan.$host_tag"
		: (-f "$test_base.inspect.plan" ? "$test_base.inspect.plan" : '');

	my ($need_helper_object, $need_helper_static, $need_helper_shared) = (0, 0, 0);
	for my $flag (@link_flags)
	{
		$need_helper_object = 1 if $flag =~ m/\.o$/;
		$need_helper_static = 1 if $flag =~ m/\.a$/;
		$need_helper_shared = 1 if $flag =~ m/(?:\.__SHARED_EXT__|\.so)$/;
	}
	$need_helper_object = 1 if $need_helper_static || $need_helper_shared;

	my $libdir = '';
	my $helper_status = 0;
	my @helper_provider_files;
	my @helper_referencer_files;
	my @helper_sources =
		grep { $_ =~ m/\.(?:c|cc|cpp|cxx)$/ } glob("$test_base.lib.*");
	if (scalar(@helper_sources) != 0)
	{
		$libdir = "$test_base.$suffix.libdir";
	}

	@link_flags = map {
		my $flag = $_;
		$flag =~ s/__LIBDIR__/$libdir/g if $libdir ne '';
		$flag =~ s/__SHARED_EXT__/$shared_ext/g;
		$flag;
	} @link_flags;

	my @objfiles;
	for my $src (@srcfiles)
	{
		my ($index) = ($src =~ m/\.([^.]+)$/);
		push @objfiles, "$test_base.$suffix.$index.o";
	}

	return {
		suffix => $suffix,
		test => $test,
		test_base => $test_base,
		build_timeout => $build_timeout,
		env => \%worker_env,
		host_cxx_str => $host_cxx_str,
		host_cc_str => $host_cc_str,
		host_cxx => \@host_cxx,
		host_cc => \@host_cc,
		stdlib_flags => \@stdlib_flags,
		link_driver => \@link_driver,
		shared_ext => $shared_ext,
		impl_stdout => $impl_stdout,
		impl_stderr => $impl_stderr,
		link_command => $link_command,
		link_verbose => $link_verbose,
		nm_report => $nm_report,
		compile_flags => \@compile_flags,
		link_flags => \@link_flags,
		run_args => \@run_args,
		inspect_cmd_file => $inspect_cmd_file,
		inspect_expect_file => $inspect_expect_file,
		inspect_facts_file => $inspect_facts_file,
		inspect_plan_file => $inspect_plan_file,
		need_helper_object => $need_helper_object,
		need_helper_static => $need_helper_static,
		need_helper_shared => $need_helper_shared,
		libdir => $libdir,
		helper_sources => \@helper_sources,
		helper_provider_files => [],
		helper_referencer_files => [],
		srcfiles => \@srcfiles,
		objfiles => \@objfiles,
	};
}

sub helper_paths_for_source
{
	my ($ctx, $src) = @_;
	my $name = $src;
	$name =~ s/^\Q$ctx->{test_base}.lib.\E//;
	$name =~ s/\.[^.]+$//;
	return (
		$name,
		"$ctx->{libdir}/lib$name.o",
		"$ctx->{libdir}/lib$name.a",
		"$ctx->{libdir}/lib$name.$ctx->{shared_ext}",
	);
}

sub compile_helper_inputs
{
	my ($ctx) = @_;
	return 0 if scalar(@{$ctx->{helper_sources}}) == 0;

	mkdir($ctx->{libdir}) or die "Unable to create $ctx->{libdir}: $!";
	for my $src (@{$ctx->{helper_sources}})
	{
		my ($name, $obj, $staticlib, $sharedlib) = helper_paths_for_source($ctx, $src);
		my @compiler = $src =~ m/\.c$/
			? @{$ctx->{host_cc}}
			: (@{$ctx->{host_cxx}}, @{$ctx->{stdlib_flags}});

		if ($ctx->{need_helper_object})
		{
			my $helper_status = run_command_capture(
				cmd => [@compiler, '-fPIC', '-c', '-o', $obj, $src],
				stdout => $ctx->{impl_stdout},
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			return $helper_status if $helper_status != 0;
			push @{$ctx->{helper_provider_files}}, $obj;
			push @{$ctx->{helper_referencer_files}}, $obj;
		}
		if ($ctx->{need_helper_static})
		{
			my $helper_status = run_command_capture(
				cmd => ['ar', 'rcs', $staticlib, $obj],
				stdout => $ctx->{impl_stdout},
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			return $helper_status if $helper_status != 0;
			push @{$ctx->{helper_provider_files}}, $staticlib;
		}
		if ($ctx->{need_helper_shared})
		{
			my @cmd = (@{$ctx->{host_cc}}, '-shared', '-o', $sharedlib, $obj);
			my $helper_status = run_command_capture(
				cmd => \@cmd,
				stdout => $ctx->{impl_stdout},
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			return $helper_status if $helper_status != 0;
			push @{$ctx->{helper_provider_files}}, $sharedlib;
		}
	}
	return 0;
}

sub record_existing_helper_inputs
{
	my ($ctx) = @_;
	for my $src (@{$ctx->{helper_sources}})
	{
		my ($name, $obj, $staticlib, $sharedlib) = helper_paths_for_source($ctx, $src);
		if ($ctx->{need_helper_object} && -e $obj)
		{
			push @{$ctx->{helper_provider_files}}, $obj;
			push @{$ctx->{helper_referencer_files}}, $obj;
		}
		push @{$ctx->{helper_provider_files}}, $staticlib
			if $ctx->{need_helper_static} && -e $staticlib;
		push @{$ctx->{helper_provider_files}}, $sharedlib
			if $ctx->{need_helper_shared} && -e $sharedlib;
	}
}

sub compile_one_test
{
	my ($suffix, $test, $worker_out, $worker_in, $host_tag) = @_;
	note_progress_state('prepare', $test);
	my $ctx = build_test_context($suffix, $test, $host_tag);
	remove_previous_outputs($ctx->{test_base}, $suffix);
	write_file($ctx->{impl_stdout}, '');
	write_file($ctx->{impl_stderr}, '');

	if (scalar(@{$ctx->{srcfiles}}) == 0)
	{
		write_numeric_status(impl_status_file($ctx), 1);
		return;
	}

	my $helper_status = compile_helper_inputs($ctx);
	if ($helper_status != 0)
	{
		write_numeric_status(impl_status_file($ctx), $helper_status);
		remove_tree($ctx->{libdir}) if $ctx->{libdir} ne '';
		return;
	}

	my $compile_status = 0;
	for my $index (0 .. $#{$ctx->{srcfiles}})
	{
		my $src = $ctx->{srcfiles}->[$index];
		my $objfile = $ctx->{objfiles}->[$index];
		note_progress_state('compile', $test);
		$compile_status = submit_cli_request(
			$worker_in,
			$worker_out,
			$ctx->{impl_stdout},
			$ctx->{impl_stderr},
			$ctx->{env},
			@{$ctx->{compile_flags}},
			'-c',
			'-o',
			$objfile,
			$src);
		last if $compile_status != 0;
	}

	if ($compile_status != 0)
	{
		write_numeric_status(impl_status_file($ctx), $compile_status);
		unlink(@{$ctx->{objfiles}});
		remove_tree($ctx->{libdir}) if $ctx->{libdir} ne '';
		return;
	}
}

sub finish_one_test
{
	my ($suffix, $test, $host_tag) = @_;
	my $ctx = build_test_context($suffix, $test, $host_tag);
	return if -f impl_status_file($ctx);
	record_existing_helper_inputs($ctx);

	for my $objfile (@{$ctx->{objfiles}})
	{
		if (!-f $objfile)
		{
			write_numeric_status(impl_status_file($ctx), 1);
			remove_tree($ctx->{libdir}) if $ctx->{libdir} ne '';
			return;
		}
	}

	my @link_cmd = @{$ctx->{link_driver}};
	push @link_cmd, '-no-pie' if $host_tag eq 'linux';
	push @link_cmd, '-o', "$ctx->{test_base}.$suffix.program", @{$ctx->{objfiles}};
	push @link_cmd, "-Wl,-rpath,$ctx->{libdir}" if $ctx->{libdir} ne '';
	push @link_cmd, @{$ctx->{stdlib_flags}};
	push @link_cmd, @{$ctx->{link_flags}};
	write_file($ctx->{link_command}, join(' ', map { shell_quote($_) } @link_cmd) . "\n");

	note_progress_state('link', $test);
	my $impl_status = run_command_capture(
		cmd => \@link_cmd,
		stdout => $ctx->{impl_stdout},
		stderr => $ctx->{impl_stderr},
		env => $ctx->{env},
		timeout => $ctx->{build_timeout},
	);
	write_numeric_status(impl_status_file($ctx), $impl_status);

	if ($impl_status != 0)
	{
		my @verbose_link_cmd = @link_cmd;
		splice(@verbose_link_cmd, 1, 0, '-v');
		write_file($ctx->{link_verbose}, '');
		run_command_capture(
			cmd => \@verbose_link_cmd,
			stdout => $ctx->{link_verbose},
			stderr => $ctx->{link_verbose},
			env => $ctx->{env},
			timeout => $ctx->{build_timeout},
		);

		my @explicit_provider_flags =
			grep { $_ =~ m/\.(?:o|a|so)$/ && -e $_ } @{$ctx->{link_flags}};
		my @nm_referencers = (
			@{$ctx->{objfiles}},
			@{$ctx->{helper_referencer_files}},
			grep { $_ =~ m/\.o$/ } @explicit_provider_flags,
		);
		my @nm_providers = (
			@{$ctx->{objfiles}},
			@{$ctx->{helper_provider_files}},
			@explicit_provider_flags,
		);
		system(
			'perl',
			"$FindBin::Bin/write_unresolved_symbol_report.pl",
			'--output',
			$ctx->{nm_report},
			'--host-cxx',
			$ctx->{host_cxx_str},
			map(('--referencer', $_), @nm_referencers),
			map(('--provider', $_), @nm_providers),
		);
	}

	if ($impl_status == 0)
	{
		my $inspect_file = "$ctx->{test_base}.$suffix.inspect";
		if ($ctx->{inspect_facts_file} ne '')
		{
			note_progress_state('inspect', $test);
			$impl_status = run_command_capture(
				cmd => ['perl',
				        "$FindBin::Bin/dump_host_eh_object_facts.pl",
				        '--plan',
				        $ctx->{inspect_facts_file},
				        @{$ctx->{objfiles}}],
				stdout => $inspect_file,
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			write_numeric_status(impl_status_file($ctx), $impl_status)
				if $impl_status != 0;
		}
		elsif ($ctx->{inspect_expect_file} ne '')
		{
			note_progress_state('inspect', $test);
			$impl_status = run_command_capture(
				cmd => ['perl',
				        "$FindBin::Bin/check_object_expectations.pl",
				        $ctx->{inspect_expect_file},
				        @{$ctx->{objfiles}}],
				stdout => $inspect_file,
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			write_numeric_status(impl_status_file($ctx), $impl_status)
				if $impl_status != 0;
		}
		elsif ($ctx->{inspect_plan_file} ne '' && ($suffix eq 'ref' || -f "$ctx->{test_base}.ref.inspect"))
		{
			note_progress_state('inspect', $test);
			$impl_status = run_command_capture(
				cmd => ['perl',
				        "$FindBin::Bin/check_object_expectations.pl",
				        '--record',
				        $ctx->{inspect_plan_file},
				        @{$ctx->{objfiles}}],
				stdout => $inspect_file,
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			write_numeric_status(impl_status_file($ctx), $impl_status)
				if $impl_status != 0;
		}
		elsif ($ctx->{inspect_cmd_file} ne '')
		{
			note_progress_state('inspect', $test);
			my $inspect_cmd = read_text_file($ctx->{inspect_cmd_file});
			for my $idx (0 .. $#{$ctx->{objfiles}})
			{
				my $n = $idx + 1;
				my $obj = $ctx->{objfiles}->[$idx];
				$inspect_cmd = replace_all_literal($inspect_cmd,
				                                  "__OBJ${n}__",
				                                  $obj);
			}
			$inspect_cmd = replace_all_literal($inspect_cmd,
			                                  '__PROGRAM__',
			                                  "$ctx->{test_base}.$suffix.program");
			$inspect_cmd = replace_all_literal($inspect_cmd,
			                                  '__SHARED_EXT__',
			                                  $ctx->{shared_ext});
			$inspect_cmd = replace_all_literal($inspect_cmd,
			                                  '__LIBDIR__',
			                                  $ctx->{libdir}) if $ctx->{libdir} ne '';
			$impl_status = run_command_capture(
				cmd => ['bash', '-lc', $inspect_cmd],
				stdout => $inspect_file,
				stderr => $ctx->{impl_stderr},
				env => $ctx->{env},
				timeout => $ctx->{build_timeout},
			);
			write_numeric_status(impl_status_file($ctx), $impl_status)
				if $impl_status != 0;
		}
	}

	if ($impl_status == 0)
	{
		unlink(@{$ctx->{objfiles}});
		note_progress_state('run', $test);
		my $program_status = run_command_capture(
			cmd => ["$ctx->{test_base}.$suffix.program", @{$ctx->{run_args}}],
			stdout => "$ctx->{test_base}.$suffix.program.stdout",
			stderr => "$ctx->{test_base}.$suffix.program.stderr",
			stdin => (-f "$ctx->{test_base}.stdin" ? "$ctx->{test_base}.stdin" : undef),
			env => $ctx->{env},
			timeout => get_timeout_from_env("CPPGM_PROGRAM_TEST_TIMEOUT_SEC", 10),
		);
		write_numeric_status("$ctx->{test_base}.$suffix.program.exit_status", $program_status);
		unlink("$ctx->{test_base}.$suffix.program", "$ctx->{test_base}.$suffix.program.stderr");
		unlink($ctx->{impl_stdout}, $ctx->{impl_stderr}, $ctx->{link_command}, $ctx->{link_verbose}, $ctx->{nm_report});
		remove_tree($ctx->{libdir}) if $ctx->{libdir} ne '';
	}
	else
	{
		unlink(glob("$ctx->{test_base}.$suffix.program"));
		unlink(glob("$ctx->{test_base}.$suffix.program.exit_status"));
		unlink(glob("$ctx->{test_base}.$suffix.program.stdout"));
		unlink(glob("$ctx->{test_base}.$suffix.program.stderr"));
	}
}

sub run_hostinterop_compile_tests
{
	my ($app, $suffix, $tests, $verbose, $host_tag) = @_;
	my ($worker_pid, $worker_out, $worker_in) = open_worker($app);
	for my $test (@{$tests})
	{
		print "Running $test...\n" if $verbose;
		compile_one_test($suffix, $test, $worker_out, $worker_in, $host_tag);
	}
	close_worker($worker_pid, $worker_out, $worker_in);
}

sub run_hostinterop_finish_tests
{
	my ($suffix, $tests, $verbose, $host_tag) = @_;
	for my $test (@{$tests})
	{
		finish_one_test($suffix, $test, $host_tag);
	}
}

sub run_hostinterop_tests
{
	my ($app, $suffix, $tests, $verbose, $host_tag) = @_;
	run_hostinterop_compile_tests($app, $suffix, $tests, $verbose, $host_tag);
	run_hostinterop_finish_tests($suffix, $tests, $verbose, $host_tag);
}

sub run_sharded_phase
{
	my ($shards, $callback) = @_;
	my @pids;
	for my $shard (@{$shards})
	{
		next if scalar(@{$shard}) == 0;
		my $pid = fork();
		die "fork failed: $!" if !defined($pid);
		if ($pid == 0)
		{
			$callback->($shard);
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
	return $failed;
}

my ($app, $suffix, $tests_root) = @ARGV;
ensure_test_app_available($app, $suffix, $tests_root);
my @tests = collect_tests($tests_root, qr/\.t$/);
my $verbose = $ENV{VERBOSE} || $ENV{CPGM_TEST_VERBOSE};
my $keep_going = $ENV{KEEP_GOING};
my $assignment = basename(getcwd());
my $host_tag = host_platform_tag();
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
	run_hostinterop_tests($app, $suffix, \@tests, $verbose, $host_tag);
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

my $failed = run_sharded_phase(\@shards, sub {
	my ($shard) = @_;
	run_hostinterop_compile_tests($app, $suffix, $shard, $verbose, $host_tag);
});
if (!$failed)
{
	$failed = run_sharded_phase(\@shards, sub {
		my ($shard) = @_;
		run_hostinterop_finish_tests($suffix, $shard, $verbose, $host_tag);
	});
}

clear_progress_state();
exit($failed ? 1 : 0);
