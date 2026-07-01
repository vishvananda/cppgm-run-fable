package CppgmBatchWorker;

use strict;
use warnings;

use Exporter 'import';
use Errno qw(EACCES EPERM ESRCH);
use File::Find;
use Fcntl qw(F_GETFD F_SETFD FD_CLOEXEC);
use IPC::Open2;
use IO::Handle;
use POSIX qw(setpgid WNOHANG);
use Text::ParseWords qw(shellwords);

our @EXPORT_OK = qw(
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
  write_named_status
  write_named_status_code
  write_numeric_status
  system_status_to_exit_code
);

my %worker_has_process_group;

sub progress_file_path
{
	my $path = $ENV{CPPGM_PROGRESS_FILE};
	return undef if !defined($path) || $path eq '';
	return $path;
}

sub sanitize_progress_field
{
	my ($value) = @_;
	$value = '' if !defined($value);
	$value =~ s/[\r\n\t]/ /g;
	return $value;
}

sub note_progress_state
{
	my ($phase, $test) = @_;
	my $path = progress_file_path();
	return if !defined($path);

	my $tmp = "$path.$$." . int(rand(1000000)) . ".tmp";
	if (open(my $fh, '>', $tmp))
	{
		print $fh join("\t",
		               time(),
		               $$,
		               sanitize_progress_field($phase),
		               sanitize_progress_field($test)) . "\n";
		close($fh);
		rename($tmp, $path) or unlink($tmp);
	}
}

sub clear_progress_state
{
	my $path = progress_file_path();
	return if !defined($path);
	unlink($path);
}

sub collect_tests
{
	my ($spec, $pattern) = @_;
	my @roots = shellwords($spec);
	die "Test path '$spec' does not exist\n" if scalar(@roots) == 0;

	my @tests;
	my %seen;
	for my $term (@roots)
	{
		my @matches = $term =~ /[*?\[]/ ? glob($term) : ($term);
		die "Test path '$term' does not exist\n" if scalar(@matches) == 0;
		for my $root (sort @matches)
		{
			die "Test path '$root' does not exist\n" if !-e $root;
			my @found;
			if (-f $root)
			{
				@found = $root =~ $pattern ? ($root) : ();
			}
			elsif (-d $root)
			{
				find(sub {
					return if !-f $_;
					push @found, $File::Find::name if $File::Find::name =~ $pattern;
				}, $root);
				@found = sort @found;
			}

			for my $test (@found)
			{
				next if $seen{$test}++;
				push @tests, $test;
			}
		}
	}
	return @tests;
}

sub get_timeout_from_env
{
	my ($name, $default) = @_;
	my $value = $ENV{$name};
	return $default if !defined($value);
	return $default if $value !~ m/^\d+$/;
	return $value > 0 ? $value : $default;
}

sub env_flag_enabled
{
	my ($name) = @_;
	return 0 if !defined($ENV{$name});
	my $value = $ENV{$name};
	return 0 if $value eq '';
	return 0 if $value eq '0';
	return 0 if $value =~ /^(?:false|no|off)$/i;
	return 1;
}

sub print_test_run_summary
{
	my ($assignment, $tests_root, $tests) = @_;
	my $ntests = scalar(@{$tests});
	if (env_flag_enabled('CPPGM_CHECK_MODE'))
	{
		print "$assignment check: running $ntests test";
	}
	else
	{
		print "$assignment $tests_root: running $ntests test";
	}
	print "s" if $ntests != 1;
	print " ($tests->[0])" if env_flag_enabled('CPPGM_CHECK_MODE') && $ntests == 1;
	print "\n";
}

sub write_file
{
	my ($path, $contents) = @_;
	open(my $fh, '>', $path) or die "Unable to write $path: $!";
	print $fh $contents;
	close($fh) or die "Unable to close $path: $!";
}

sub write_numeric_status
{
	my ($path, $status) = @_;
	write_file($path, "$status\n");
}

sub write_named_status
{
	my ($path, $success) = @_;
	write_file($path, $success ? "EXIT_SUCCESS\n" : "EXIT_FAILURE\n");
}

sub write_named_status_code
{
	my ($path, $status) = @_;
	if (!defined($status))
	{
		write_file($path, "EXIT_FAILURE\n");
		return;
	}
	write_file($path, $status == 0
		? "EXIT_SUCCESS\n"
		: $status == 124
			? "EXIT_TIMEOUT\n"
			: $status == 125
				? "EXIT_OOM\n"
				: $status == 86
					? "EXIT_NOT_IMPLEMENTED\n"
					: "EXIT_FAILURE\n");
}

sub system_status_to_exit_code
{
	my ($status) = @_;
	return 1 if !defined($status) || $status == -1;
	return $status >> 8 if ($status & 127) == 0;
	return 128 + ($status & 127);
}

sub read_word_list
{
	my ($path) = @_;
	return () if !-f $path;

	open(my $fh, '<', $path) or die "Unable to read $path: $!";
	my $line = <$fh>;
	close($fh) or die "Unable to close $path: $!";
	return () if !defined($line);

	chomp($line);
	return shellwords($line);
}

sub command_exists
{
	my ($command) = @_;
	return 0 if !defined($command) || $command eq '';
	return -x $command if $command =~ m{/};
	for my $dir (split(/:/, $ENV{PATH} || ''))
	{
		next if !defined($dir) || $dir eq '';
		my $path = "$dir/$command";
		return 1 if -x $path;
	}
	return 0;
}

sub ensure_test_app_available
{
	my ($app, $suffix, $tests_root) = @_;
	return if command_exists($app);

	my $kind = $suffix eq 'ref' ? 'reference test app' : 'test app';
	my $message = "ERROR: $kind '$app' does not exist or is not executable.\n";

	if ($suffix eq 'ref')
	{
		my $regular_app = $app;
		$regular_app =~ s/-ref$//;
		my $test_arg = defined($tests_root) && $tests_root ne '' ? " TEST=$tests_root" : "";
		$message .= "No .ref files were written.\n";
		$message .= "Build/export the reference binary, or intentionally regenerate with the current compiler using:\n";
		$message .= "  make ref-test$test_arg REF_TEST_APP=$regular_app\n";
	}

	die $message;
}

sub resolve_host_command
{
	my ($explicit, @fallbacks) = @_;
	if (defined($explicit) && $explicit ne '')
	{
		my @words = shellwords($explicit);
		return @words if scalar(@words) != 0 && command_exists($words[0]);
	}
	for my $candidate (@fallbacks)
	{
		next if !defined($candidate) || $candidate eq '';
		return ($candidate) if command_exists($candidate);
	}
	return ();
}

sub read_env_file
{
	my ($path) = @_;
	my %env;
	return \%env if !-f $path;

	open(my $fh, '<', $path) or die "Unable to read $path: $!";
	while(my $line = <$fh>)
	{
		chomp($line);
		next if $line =~ m/^\s*$/;
		next if $line =~ m/^\s*#/;
		if ($line =~ m/^([A-Za-z_][A-Za-z0-9_]*)=(.*)$/)
		{
			$env{$1} = $2;
		}
	}
	close($fh) or die "Unable to close $path: $!";
	return \%env;
}

sub open_worker
{
	my ($app) = @_;
	my @app_args = shellwords($ENV{CPPGM_APP_ARGS} || '');
	my ($worker_out, $worker_in);
	my $pid = open2($worker_out,
	               $worker_in,
	               'env',
	               'WRAPPED_BATCH_STDIN=1',
	               $app,
	               @app_args,
	               '--batch-stdin');
	$worker_in->autoflush(1);
	set_cloexec($worker_in, "worker stdin");
	set_cloexec($worker_out, "worker stdout");
	$worker_has_process_group{$pid} = try_set_process_group($pid);
	return ($pid, $worker_out, $worker_in);
}

sub try_set_process_group
{
	my ($pid) = @_;
	return 0 if !defined($pid) || $pid <= 0;
	return 1 if setpgid($pid, $pid);
	return 0 if $!{EACCES} || $!{EPERM} || $!{ESRCH};
	die "Unable to set worker process group for $pid: $!";
}

sub set_cloexec
{
	my ($fh, $label) = @_;
	my $flags = fcntl($fh, F_GETFD, 0);
	die "Unable to read close-on-exec flags for $label: $!" if !defined($flags);
	fcntl($fh, F_SETFD, $flags | FD_CLOEXEC)
		or die "Unable to set close-on-exec for $label: $!";
}

sub close_worker
{
	my ($pid, $worker_out, $worker_in) = @_;
	close($worker_in) or die "Unable to close worker stdin: $!";
	close($worker_out) or die "Unable to close worker stdout: $!";
	if (wait_for_pid_exit($pid, 500))
	{
		delete $worker_has_process_group{$pid};
		die "worker exited with status $?\n" if $? != 0;
		return;
	}
	force_terminate_worker($pid);
}

sub wait_for_pid_exit
{
	my ($pid, $timeout_ms) = @_;
	while ($timeout_ms > 0)
	{
		my $res = waitpid($pid, WNOHANG);
		return 1 if $res == $pid;
		return 0 if $res < 0;
		select(undef, undef, undef, 0.01);
		$timeout_ms -= 10;
	}
	return 0;
}

sub force_terminate_worker
{
	my ($pid) = @_;
	my $has_process_group = delete $worker_has_process_group{$pid};
	kill 'TERM', $pid;
	kill 'TERM', -$pid if $has_process_group;
	if (!wait_for_pid_exit($pid, 500))
	{
		kill 'KILL', $pid;
		kill 'KILL', -$pid if $has_process_group;
		waitpid($pid, 0);
	}
}

sub encode_env
{
	my ($env) = @_;
	return '' if !defined($env);
	return join(';', map { $_ . '=' . $env->{$_} } sort keys %{$env});
}

sub submit_cli_request
{
	my ($worker_in, $worker_out, $stdout_path, $stderr_path, $env, @args) = @_;
	my @fields = ($stdout_path, $stderr_path, '-', encode_env($env), @args);
	print {$worker_in} join("\t", @fields), "\n"
		or die "Unable to write work item: $!";
	my $status = <$worker_out>;
	die "worker exited unexpectedly\n" if !defined($status);
	chomp($status);
	return 0 if $status eq 'EXIT_SUCCESS';
	return 124 if $status eq 'EXIT_TIMEOUT';
	return 125 if $status eq 'EXIT_OOM';
	return 86 if $status eq 'EXIT_NOT_IMPLEMENTED';
	return 1 if $status eq 'EXIT_FAILURE';
	return $status if $status =~ m/^\d+$/;
	die "Unexpected worker status '$status'\n";
}

sub run_command_capture
{
	my (%options) = @_;
	my @cmd = @{$options{cmd}};
	my $stdout_path = $options{stdout};
	my $stderr_path = $options{stderr};
	my $stdin_path = $options{stdin};
	my $timeout = $options{timeout};
	my $env = $options{env} || {};

	open(my $stdout_fh, '>>', $stdout_path)
		or die "Unable to write $stdout_path: $!";
	open(my $stderr_fh, '>>', $stderr_path)
		or die "Unable to write $stderr_path: $!";

	my $pid = fork();
	die "fork failed: $!" unless defined($pid);
	if ($pid == 0)
	{
		setpgid(0, 0);
		for my $name (keys %{$env})
		{
			$ENV{$name} = $env->{$name};
		}

		if (defined($stdin_path))
		{
			open(STDIN, '<', $stdin_path)
				or die "Unable to read $stdin_path: $!";
		}

		open(STDOUT, '>&', $stdout_fh)
			or die "Unable to redirect stdout: $!";
		open(STDERR, '>&', $stderr_fh)
			or die "Unable to redirect stderr: $!";

		exec {$cmd[0]} @cmd;
		die "Unable to exec $cmd[0]: $!";
	}
	my $has_process_group = try_set_process_group($pid);

	if (defined($timeout))
	{
		my $timeout_ms = $timeout > 2147483 ? 2147483000 : $timeout * 1000;
		if (!wait_for_pid_exit($pid, $timeout_ms))
		{
			kill 'TERM', $pid;
			kill 'TERM', -$pid if $has_process_group;
			if (!wait_for_pid_exit($pid, 200))
			{
				kill 'KILL', $pid;
				kill 'KILL', -$pid if $has_process_group;
				waitpid($pid, 0);
			}
			close($stdout_fh) or die "Unable to close $stdout_path: $!";
			close($stderr_fh) or die "Unable to close $stderr_path: $!";
			return 124;
		}
	}
	else
	{
		waitpid($pid, 0);
	}

	close($stdout_fh) or die "Unable to close $stdout_path: $!";
	close($stderr_fh) or die "Unable to close $stderr_path: $!";

	return $? >> 8 if ($? & 127) == 0;
	return 128 + ($? & 127);
}

1;
