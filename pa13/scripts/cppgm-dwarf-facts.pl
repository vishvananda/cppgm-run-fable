#!/usr/bin/perl
use strict;
use warnings;

my ($objfile, $include_loc) = @ARGV;
if (!defined($objfile) || !defined($include_loc)) {
  die "usage: $0 <object-file> <include-loc>\n";
}

sub find_command {
  my ($name) = @_;
  for my $dir (split(/:/, $ENV{PATH} || "")) {
    my $path = "$dir/$name";
    return $path if -x $path;
  }
  return undef;
}

sub run_capture {
  my (@cmd) = @_;
  open(my $fh, "-|", @cmd) or die "failed to run @cmd: $!\n";
  local $/;
  my $out = <$fh>;
  close($fh);
  return defined($out) ? $out : "";
}

sub trim {
  my ($text) = @_;
  $text = "" if !defined($text);
  $text =~ s/^\s+//;
  $text =~ s/\s+\z//;
  return $text;
}

sub clean_attr_value {
  my ($value) = @_;
  $value = trim($value);
  $value =~ s/^\([^)]*\):\s*//;
  return $1 if $value =~ /^"([^"]*)"/;
  return $1 if $value =~ /^\("([^"]*)"\)/;
  $value =~ s/\s+\([^)]*\)\s*\z//;
  return trim($value);
}

sub parse_number {
  my ($value) = @_;
  return undef if !defined($value);
  return hex($1) if $value =~ /0x([0-9a-fA-F]+)/;
  return int($1) if $value =~ /([0-9]+)/;
  return undef;
}

sub add_unique {
  my ($array, $seen, $value) = @_;
  return if !defined($value) || $value eq "";
  return if $seen->{$value}++;
  push(@$array, $value);
}

sub new_facts {
  return {
    debug_info => 0,
    debug_line => 0,
    debug_loc => undef,
    compile_units => [],
    compile_seen => {},
    subprograms => [],
    parameters => [],
    variables => [],
    lines => {},
  };
}

sub parse_info_common_line {
  my ($facts, $current_ref, $line, $style) = @_;

  if ($line =~ /DW_TAG_([A-Za-z0-9_]+)/) {
    my $tag = $1;
    if ($tag eq "compile_unit") {
      $$current_ref = { kind => "compile_unit" };
    } elsif ($tag eq "subprogram") {
      my $entry = { name => undef, line => undef };
      push(@{$facts->{subprograms}}, $entry);
      $$current_ref = { kind => "subprogram", entry => $entry };
    } elsif ($tag eq "formal_parameter") {
      my $entry = { name => undef, location => 0 };
      push(@{$facts->{parameters}}, $entry);
      $$current_ref = { kind => "parameter", entry => $entry };
    } elsif ($tag eq "variable") {
      my $entry = { name => undef, location => 0 };
      push(@{$facts->{variables}}, $entry);
      $$current_ref = { kind => "variable", entry => $entry };
    } else {
      $$current_ref = undef;
    }
    return;
  }

  my $current = $$current_ref;
  return if !$current;

  my $name;
  if ($style eq "readelf" && $line =~ /DW_AT_name\s*:\s*(.*)\z/) {
    $name = clean_attr_value($1);
  } elsif ($style eq "dwarfdump" && $line =~ /DW_AT_name\s+\((.*)\)\s*\z/) {
    $name = clean_attr_value("($1)");
  }
  if (defined($name)) {
    if ($current->{kind} eq "compile_unit") {
      add_unique($facts->{compile_units}, $facts->{compile_seen}, $name);
    } elsif ($current->{entry}) {
      $current->{entry}->{name} = $name;
    }
    return;
  }

  my $decl_line;
  if ($style eq "readelf" && $line =~ /DW_AT_decl_line\s*:\s*(.*)\z/) {
    $decl_line = parse_number($1);
  } elsif ($style eq "dwarfdump" && $line =~ /DW_AT_decl_line\s+\((.*)\)\s*\z/) {
    $decl_line = parse_number($1);
  }
  if (defined($decl_line) && $current->{kind} eq "subprogram") {
    $current->{entry}->{line} = $decl_line;
    return;
  }

  if ($line =~ /DW_AT_location\b/ &&
      ($current->{kind} eq "parameter" || $current->{kind} eq "variable")) {
    $current->{entry}->{location} = 1;
  }
}

sub parse_readelf {
  my ($objfile, $include_loc) = @_;
  my $facts = new_facts();
  my $sections = run_capture("readelf", "-S", $objfile);
  $facts->{debug_info} = ($sections =~ /\b\.debug_info\b/) ? 1 : 0;
  $facts->{debug_line} = ($sections =~ /\b\.debug_line\b/) ? 1 : 0;
  $facts->{debug_loc} = ($sections =~ /\b\.debug_loc(?:lists)?\b/) ? 1 : 0
    if $include_loc;

  if ($facts->{debug_info}) {
    my $info = run_capture("readelf", "--debug-dump=info", $objfile);
    my $current;
    for my $line (split(/\n/, $info)) {
      parse_info_common_line($facts, \$current, $line, "readelf");
    }
  }

  if ($facts->{debug_line}) {
    my $line_info = run_capture("readelf", "--debug-dump=decodedline", $objfile);
    for my $line (split(/\n/, $line_info)) {
      if ($line =~ /^\S.*\s+([0-9]+)\s+(?:0x[0-9a-fA-F]+|[0-9a-fA-F]+)\b/) {
        $facts->{lines}->{$1} = 1;
      }
    }
  }

  return $facts;
}

sub parse_dwarfdump {
  my ($objfile, $include_loc, $tool) = @_;
  my @sections = ("--debug-line", "--debug-info");
  push(@sections, "--debug-loc") if $include_loc;
  my $dump = run_capture($tool, @sections, $objfile);
  my $facts = new_facts();
  $facts->{debug_info} = ($dump =~ /DW_TAG_compile_unit/) ? 1 : 0;
  $facts->{debug_line} = ($dump =~ /^\s*(?:0x[0-9a-fA-F]+|<hex>)\s+[0-9]+\s+[0-9]+\s+[0-9]+\b/m) ? 1 : 0;
  $facts->{debug_loc} = ($dump =~ /DW_OP_/) ? 1 : 0 if $include_loc;

  my $current;
  for my $line (split(/\n/, $dump)) {
    parse_info_common_line($facts, \$current, $line, "dwarfdump");
    if ($line =~ /^\s*(?:0x[0-9a-fA-F]+|<hex>)\s+([0-9]+)\s+[0-9]+\s+[0-9]+\b/) {
      $facts->{lines}->{$1} = 1;
    }
  }

  return $facts;
}

sub prune_unnamed {
  my ($array) = @_;
  @$array = grep { defined($_->{name}) && $_->{name} ne "" } @$array;
}

sub print_facts {
  my ($facts, $include_loc) = @_;
  prune_unnamed($facts->{subprograms});
  prune_unnamed($facts->{parameters});
  prune_unnamed($facts->{variables});

  print "dwarf-facts 1\n";
  print "object native\n";
  print "debug_info ", ($facts->{debug_info} ? "present" : "absent"), "\n";
  print "debug_line ", ($facts->{debug_line} ? "present" : "absent"), "\n";
  if ($include_loc) {
    print "debug_loc ", ($facts->{debug_loc} ? "present" : "absent"), "\n";
  } else {
    print "debug_loc not-requested\n";
  }

  for my $unit (@{$facts->{compile_units}}) {
    print "compile_unit $unit\n";
  }
  for my $sub (@{$facts->{subprograms}}) {
    my $line = defined($sub->{line}) ? $sub->{line} : "unknown";
    print "subprogram $sub->{name} line $line\n";
  }
  for my $param (@{$facts->{parameters}}) {
    print "parameter $param->{name} location ", ($param->{location} ? "present" : "absent"), "\n";
  }
  for my $var (@{$facts->{variables}}) {
    print "variable $var->{name} location ", ($var->{location} ? "present" : "absent"), "\n";
  }
  for my $line (sort { $a <=> $b } keys(%{$facts->{lines}})) {
    print "line $line\n";
  }
}

my $facts;
if (find_command("readelf")) {
  $facts = parse_readelf($objfile, $include_loc);
} elsif (my $dwarfdump = find_command("dwarfdump")) {
  $facts = parse_dwarfdump($objfile, $include_loc, $dwarfdump);
} elsif (my $llvm_dwarfdump = find_command("llvm-dwarfdump")) {
  $facts = parse_dwarfdump($objfile, $include_loc, $llvm_dwarfdump);
} else {
  die "missing readelf, dwarfdump, or llvm-dwarfdump\n";
}

print_facts($facts, $include_loc);
