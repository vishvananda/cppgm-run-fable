#!/usr/bin/env perl
use strict;
use warnings;

use Cwd qw(abs_path);
use File::Find qw(find);
use FindBin qw($Bin);
use File::Temp qw(tempdir);
use Text::ParseWords qw(shellwords);

sub usage {
  die "usage: run_object_lowir_roundtrip_tests.pl [--debuginfo] --app APP (--test-root DIR | --test FILE)...\n";
}

my $app = "../dev/cppgm++";
my $debuginfo = 0;
my @roots;
my @tests;
my $repo_root = abs_path("$Bin/../..");
my $cwd = abs_path(".");

while(@ARGV) {
  my $arg = shift @ARGV;
  if($arg eq "--app") {
    usage() unless @ARGV;
    $app = shift @ARGV;
  } elsif($arg eq "--test-root") {
    usage() unless @ARGV;
    push @roots, shift @ARGV;
  } elsif($arg eq "--test") {
    usage() unless @ARGV;
    push @tests, shift @ARGV;
  } elsif($arg eq "--debuginfo") {
    $debuginfo = 1;
  } elsif($arg eq "-h" || $arg eq "--help") {
    usage();
  } else {
    die "unknown argument: $arg\n";
  }
}

sub collect_tests {
  my @out;
  for my $root (@roots) {
    if(-f $root) {
      push @out, $root;
      next;
    }
    next unless -d $root;
    my @found;
    find(
      {
        wanted => sub {
          return unless -f $_;
          return unless /\.(?:cpp|t)\z/;
          push @found, $File::Find::name;
        },
        no_chdir => 1,
      },
      $root);
    push @out, sort @found;
  }
  push @out, @tests;

  my %seen;
  return grep { !$seen{$_}++ } @out;
}

sub read_flags {
  my ($path) = @_;
  return () unless -f $path;
  open(my $fh, "<", $path) or die "open $path: $!\n";
  local $/;
  my $text = <$fh>;
  close($fh) or die "close $path: $!\n";
  return () unless defined($text) && $text =~ /\S/;
  return shellwords($text);
}

sub add_flag_paths_for {
  my ($paths, $path) = @_;
  if($path =~ /\.t\z/) {
    (my $base_flags = $path) =~ s/\.t\z/.compile.flags/;
    push @$paths, $base_flags;
  }
  if($path =~ /\.t\.\d+\z/) {
    (my $base_flags = $path) =~ s/\.t\.\d+\z/.compile.flags/;
    push @$paths, $base_flags;
  }
  push @$paths, "$path.compile.flags";
}

sub compile_flags {
  my ($test, $source) = @_;
  my @paths;
  add_flag_paths_for(\@paths, $test);
  add_flag_paths_for(\@paths, $source);
  for my $path ($test, $source) {
    my $real = abs_path($path);
    add_flag_paths_for(\@paths, $real) if defined $real && $real ne $path;
  }

  my %seen;
  my @flags;
  for my $path (grep { !$seen{$_}++ } @paths) {
    push @flags, read_flags($path);
  }
  return @flags;
}

sub harness_sources {
  my ($test) = @_;
  if($test =~ /\.t\z/) {
    my @numbered;
    for my $candidate (glob("$test.*")) {
      next unless $candidate =~ /\.t\.(\d+)\z/;
      push @numbered, [$1, $candidate];
    }
    my @sources = map { $_->[1] }
      sort { $a->[0] <=> $b->[0] || $a->[1] cmp $b->[1] } @numbered;
    return @sources if @sources;
  }
  return ($test);
}

sub run_command {
  my (@cmd) = @_;
  system { $cmd[0] } @cmd;
  my $status = $?;
  return undef if $status == 0;
  my $exit_code = $status & 127 ? 128 + ($status & 127) : ($status >> 8);
  return "command failed with exit status $exit_code:\n  " . join(" ", @cmd) . "\n";
}

sub read_bytes {
  my ($path) = @_;
  open(my $fh, "<:raw", $path) or die "open $path: $!\n";
  local $/;
  my $data = <$fh>;
  close($fh) or die "close $path: $!\n";
  return defined($data) ? $data : "";
}

sub object_summary {
  my ($path) = @_;
  open(my $fh, "-|", "nm", "-a", $path) or return "";
  my @lines = grep { /\S/ } <$fh>;
  close($fh);
  @lines = sort @lines;
  splice(@lines, 80) if @lines > 80;
  return join("", @lines);
}

sub safe_name {
  my ($path) = @_;
  $path =~ s/[^A-Za-z0-9]/_/g;
  return $path;
}

sub check_source {
  my ($test, $source, $temp) = @_;
  die "missing object-roundtrip test source: $source\n" unless -f $source;

  my @extra_flags = compile_flags($test, $source);
  my $name = safe_name($source);
  my @modes = $debuginfo
    ? (["-gline-tables-only", "-O0"], ["-gline-tables-only", "-O1"])
    : ([undef, "-O0"], [undef, "-O1"], [undef, "-O2"]);
  for my $mode (@modes) {
    my ($debug_flag, $opt) = @$mode;
    my $debug_label = defined($debug_flag) ? $debug_flag : "nodebug";
    my $mode_name = safe_name("$debug_label.$opt");
    my $direct = "$temp/$name.$mode_name.direct.o";
    my $lowir = "$temp/$name.$mode_name.lowir";
    my $from_lowir = "$temp/$name.$mode_name.from-lowir.o";

    my @debug_flags = defined($debug_flag) ? ($debug_flag) : ("-g0");
    my @direct_cmd = ($app, "-c", @extra_flags, @debug_flags, $opt);
    my $direct_error = run_command(@direct_cmd, "-o", $direct, $source);
    return $direct_error if defined $direct_error;
    my $emit_error = run_command($app,
                                 "--emit-lowir",
                                 @extra_flags,
                                 @debug_flags,
                                 "-O0",
                                 "-o",
                                 $lowir,
                                 $source);
    return $emit_error if defined $emit_error;
    my $from_lowir_error = run_command($app,
                                       "-c",
                                       @extra_flags,
                                       @debug_flags,
                                       $opt,
                                       "-o",
                                       $from_lowir,
                                       $lowir);
    return $from_lowir_error if defined $from_lowir_error;

    my $direct_bytes = read_bytes($direct);
    my $from_lowir_bytes = read_bytes($from_lowir);
    next if $direct_bytes eq $from_lowir_bytes;

    return "object differs after compiling serialized LowIR: $source $debug_label $opt\n"
      . "direct bytes: " . length($direct_bytes) . "\n"
      . "from-lowir bytes: " . length($from_lowir_bytes) . "\n"
      . "direct symbols:\n" . object_summary($direct)
      . "from-lowir symbols:\n" . object_summary($from_lowir);
  }
  return undef;
}

sub check_one {
  my ($test, $temp) = @_;
  die "missing object-roundtrip test source: $test\n" unless -f $test;

  for my $source (harness_sources($test)) {
    my $error = check_source($test, $source, $temp);
    return $error if defined $error;
  }
  return undef;
}

sub append_keep_going_summary {
  my ($passed, $total, $failed) = @_;
  if(open(my $fh, '>>', "$repo_root/.test_counts")) {
    print $fh "$passed $total\n";
    close($fh);
  }
  if($failed) {
    system('touch', "$cwd/.test_failed");
  }
}

my @selected = collect_tests();
die "no object-roundtrip tests selected\n" unless @selected;

print "pa37 object-roundtrip";
print " debuginfo" if $debuginfo;
print ": running ", scalar(@selected), " test";
print "s" if @selected != 1;
print "\n";

my $temp = tempdir("cppgm-object-lowir-roundtrip.XXXXXX", TMPDIR => 1, CLEANUP => 1);
my $passed = 0;
my $failed = 0;
my $keep_going = $ENV{KEEP_GOING};
for my $test (@selected) {
  my $error = check_one($test, $temp);
  if(defined $error) {
    print STDERR "$test: $error";
    print STDERR "\n" unless $error =~ /\n\z/;
    $failed = 1;
    last unless $keep_going;
    next;
  }
  ++$passed;
}

append_keep_going_summary($passed, scalar(@selected), $failed) if $keep_going;

if($failed) {
  print "pa37 object-roundtrip";
  print " debuginfo" if $debuginfo;
  print ": FAIL ($passed/", scalar(@selected), ")\n";
  exit 1;
}

print "pa37 object-roundtrip";
print " debuginfo" if $debuginfo;
print ": PASS (", scalar(@selected), "/", scalar(@selected), ")\n";
