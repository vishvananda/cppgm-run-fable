#!/usr/bin/env perl

use strict;
use warnings;

use POSIX qw(WNOHANG setpgid sysconf _SC_PAGESIZE);
use Time::HiRes qw(time);

sub usage
{
	die "Usage: run_with_timeout.pl --timeout-sec SEC [--label LABEL] [--max-rss-kb KB] -- command [args...]\n";
}

my $DEFAULT_MAX_RSS_KB = 8 * 1024 * 1024;
my $timeout_sec;
my $max_rss_kb;
my $label = 'command';
while (@ARGV)
{
	my $arg = shift @ARGV;
	if ($arg eq '--')
	{
		last;
	}
	elsif ($arg eq '--timeout-sec')
	{
		usage() if !@ARGV;
		$timeout_sec = shift @ARGV;
	}
	elsif ($arg eq '--label')
	{
		usage() if !@ARGV;
		$label = shift @ARGV;
	}
	elsif ($arg eq '--max-rss-kb')
	{
		usage() if !@ARGV;
		$max_rss_kb = shift @ARGV;
	}
	else
	{
		usage();
	}
}

usage() if !@ARGV;

sub parse_nonnegative_integer
{
	my ($name, $value) = @_;
	die "$name must be a nonnegative integer\n"
		if !defined($value) || $value !~ m/^\d+$/;
	return int($value);
}

$timeout_sec = parse_nonnegative_integer('--timeout-sec', $timeout_sec)
	if defined($timeout_sec);

if (!defined($max_rss_kb))
{
	if (defined($ENV{CPPGM_RUN_MAX_RSS_KB}))
	{
		$max_rss_kb =
			parse_nonnegative_integer('CPPGM_RUN_MAX_RSS_KB',
			                          $ENV{CPPGM_RUN_MAX_RSS_KB});
	}
	else
	{
		$max_rss_kb = $DEFAULT_MAX_RSS_KB;
	}
}
else
{
	$max_rss_kb = parse_nonnegative_integer('--max-rss-kb', $max_rss_kb);
}

if ((!defined($timeout_sec) || $timeout_sec == 0) && $max_rss_kb == 0)
{
	exec {$ARGV[0]} @ARGV;
	die "Unable to exec $ARGV[0]: $!\n";
}

my $pid = fork();
die "fork failed: $!\n" if !defined($pid);
if ($pid == 0)
{
	setpgid(0, 0);
	exec {$ARGV[0]} @ARGV;
	die "Unable to exec $ARGV[0]: $!\n";
}

setpgid($pid, $pid);
my $has_process_group = 1;
my $page_size = sysconf(_SC_PAGESIZE) || 4096;
my $page_kb = $page_size / 1024.0;

sub print_command
{
	print STDERR "Command:";
	for my $arg (@ARGV)
	{
		(my $escaped = $arg) =~ s/'/'\\''/g;
		print STDERR " '$escaped'";
	}
	print STDERR "\n";
}

sub proc_stat_fields
{
	my ($pid) = @_;
	open(my $fh, '<', "/proc/$pid/stat") or return;
	my $line = <$fh>;
	close($fh);
	return if !defined($line);
	$line =~ s/^\d+ \([^)]*\) // or return;
	return split(/\s+/, $line);
}

sub proc_process_group_rss_kb
{
	my ($process_group) = @_;
	opendir(my $dh, '/proc') or return;
	my $total = 0;
	my $found = 0;
	while (defined(my $entry = readdir($dh)))
	{
		next if $entry !~ m/^\d+$/;
		my @fields = proc_stat_fields($entry);
		next if scalar(@fields) < 22;
		my $pgrp = $fields[2];
		next if !defined($pgrp) || $pgrp != $process_group;
		my $rss_pages = $fields[21];
		next if !defined($rss_pages) || $rss_pages !~ m/^-?\d+$/ || $rss_pages < 0;
		$total += int($rss_pages * $page_kb);
		$found = 1;
	}
	closedir($dh);
	return $found ? $total : undef;
}

sub ps_process_group_rss_kb
{
	my ($process_group) = @_;
	open(my $ps, '-|', 'ps', '-o', 'rss=', '-g', $process_group) or return;
	my $total = 0;
	my $found = 0;
	while (defined(my $line = <$ps>))
	{
		$line =~ s/^\s+|\s+$//g;
		next if $line !~ m/^\d+$/;
		$total += int($line);
		$found = 1;
	}
	close($ps);
	return $found ? $total : undef;
}

sub process_group_rss_kb
{
	my ($process_group) = @_;
	my $rss = proc_process_group_rss_kb($process_group);
	return $rss if defined($rss);
	return ps_process_group_rss_kb($process_group);
}

sub wait_for_child
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

sub terminate_child
{
	my ($pid, $has_process_group) = @_;
	kill 'TERM', $pid;
	kill 'TERM', -$pid if $has_process_group;
	if (!wait_for_child($pid, 2000))
	{
		kill 'KILL', $pid;
		kill 'KILL', -$pid if $has_process_group;
		waitpid($pid, 0);
	}
}

sub monitor_child
{
	my ($pid, $timeout_sec, $max_rss_kb) = @_;
	my $deadline = defined($timeout_sec) && $timeout_sec > 0 ?
		time() + $timeout_sec : undef;
	my $next_rss_check = 0;
	while (1)
	{
		my $res = waitpid($pid, WNOHANG);
		return ('exit', 0) if $res == $pid;
		return ('exit', 0) if $res < 0;

		my $now = time();
		if (defined($deadline) && $now >= $deadline)
		{
			return ('timeout', 0);
		}
		if ($max_rss_kb > 0 && $now >= $next_rss_check)
		{
			my $rss_kb = process_group_rss_kb($pid);
			if (defined($rss_kb) && $rss_kb > $max_rss_kb)
			{
				return ('rss', $rss_kb);
			}
			$next_rss_check = $now + 1.0;
		}

		my $sleep_sec = 0.05;
		if (defined($deadline))
		{
			my $remaining = $deadline - $now;
			$sleep_sec = $remaining if $remaining > 0 && $remaining < $sleep_sec;
		}
		select(undef, undef, undef, $sleep_sec);
	}
}

$SIG{INT} = sub {
	terminate_child($pid, $has_process_group);
	exit 130;
};
$SIG{TERM} = sub {
	terminate_child($pid, $has_process_group);
	exit 143;
};

my ($monitor_result, $rss_kb) = monitor_child($pid, $timeout_sec, $max_rss_kb);
if ($monitor_result eq 'timeout')
{
	print STDERR "ERROR: $label timed out after ${timeout_sec}s\n";
	print_command();
	terminate_child($pid, $has_process_group);
	exit 124;
}
elsif ($monitor_result eq 'rss')
{
	print STDERR "ERROR: $label OOM: exceeded RSS limit (${rss_kb} KB > ${max_rss_kb} KB)\n";
	print_command();
	terminate_child($pid, $has_process_group);
	exit 125;
}

exit($? >> 8) if ($? & 127) == 0;
exit(128 + ($? & 127));
