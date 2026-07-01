#!/usr/bin/perl

use strict;
use warnings;
use Cwd qw(getcwd);
use File::Find;
use File::Basename qw(basename dirname);
use Scalar::Util qw(looks_like_number);
use Text::ParseWords qw(shellwords);

sub collect_tests
{
	my @tests;
	my ($spec, $pattern) = @_;
	my @roots = shellwords($spec);
	die "Test path '$spec' does not exist\n" if scalar(@roots) == 0;

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

sub getdata
{
	my ($file) = @_;
	return undef if !-e $file;
	open(my $fh, '<', $file) or return undef;
	local $/;
	my $data = <$fh>;
	close($fh);
	$data = '' if !defined($data);
	$data =~ s/\s+$//;
	return $data;
}

sub getrawdata
{
	my ($file) = @_;
	return undef if !-e $file;
	open(my $fh, '<', $file) or return undef;
	local $/;
	my $data = <$fh>;
	close($fh);
	return '' if !defined($data);
	return $data;
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

sub normalize_machine_ir
{
	my ($data) = @_;
	return undef if !defined($data);
	$data =~ s/^machine_ir x86_64 \S+$/machine_ir x86_64 <host-target>/m;
	return $data;
}

sub putrawdata
{
	my ($file, $data) = @_;
	open(my $fh, '>', $file) or die "Unable to write $file\n";
	print $fh $data;
	close($fh);
}

sub canonical_machine_ir_memory_operand
{
	my ($base, $sign, $disp, $state) = @_;
	return "[$base]" if !defined($sign) || !defined($disp);
	my $key = "$base|$sign|$disp";
	my $bucket = "$base|$sign";
	if (!exists($state->{disp_map}{$key}))
	{
		my $next = $state->{next_disp}{$bucket} || 0;
		$state->{disp_map}{$key} = $next;
		$state->{next_disp}{$bucket} = $next + 1;
	}
	my $index = $state->{disp_map}{$key};
	return "[$base$sign<off$index>]";
}

sub canonical_machine_ir_free_gpr
{
	my ($reg, $state) = @_;
	if (!exists($state->{gpr_map}{$reg}))
	{
		my $next = $state->{next_gpr};
		$state->{gpr_map}{$reg} = "<gpr$next>";
		$state->{next_gpr} = $next + 1;
	}
	return $state->{gpr_map}{$reg};
}

sub canonical_machine_ir_free_xmm
{
	my ($reg, $state) = @_;
	if (!exists($state->{xmm_map}{$reg}))
	{
		my $next = $state->{next_xmm};
		$state->{xmm_map}{$reg} = "<xmm$next>";
		$state->{next_xmm} = $next + 1;
	}
	return $state->{xmm_map}{$reg};
}

sub canonicalize_machine_ir
{
	my ($data) = @_;
	return undef if !defined($data);
	$data = normalize_machine_ir($data);
	my %state = (
		disp_map => {},
		next_disp => {},
		gpr_map => {},
		next_gpr => 0,
		xmm_map => {},
		next_xmm => 0,
	);
	$data =~ s/\[([A-Za-z_][A-Za-z0-9_]*)([+-])(\d+)\]/canonical_machine_ir_memory_operand($1, $2, $3, \%state)/ge;
	$data =~ s/\b(rbx|r10|r11|r12|r13|r14|r15)\b/canonical_machine_ir_free_gpr($1, \%state)/ge;
	$data =~ s/\b(xmm2|xmm3|xmm4|xmm5|xmm6|xmm7)\b/canonical_machine_ir_free_xmm($1, \%state)/ge;
	return $data;
}

sub compare_strict_machine_ir
{
	my ($ref, $my) = @_;
	my $ref_mir = normalize_machine_ir(getdata("$ref.mir"));
	return (1, undef, undef) if !defined($ref_mir);
	my $my_mir = normalize_machine_ir(getdata("$my.mir"));
	return (0, "ERROR: missing generated machine IR dump (.mir)", undef) if !defined($my_mir);
	return (1, undef, undef) if $ref_mir eq $my_mir;
	my $hint = "To compare strict machine IR dumps:\n\n    \$ diff " .
		rooted_path("$ref.mir") . " " . rooted_path("$my.mir") . "\n\n";
	return (0, "ERROR: strict machine IR dumps do not match (.mir)", $hint);
}

sub clear_structural_machine_ir_failure_artifacts
{
	my ($ref, $my) = @_;
	unlink("$my.cmir");
}

sub compare_structural_machine_ir
{
	my ($ref, $my) = @_;
	clear_structural_machine_ir_failure_artifacts($ref, $my);
	my $ref_raw = getrawdata("$ref.cmir");
	return (0, "ERROR: missing structural machine IR oracle (.ref.cmir)", undef) if !defined($ref_raw);
	my $my_raw = getrawdata("$my.mir");
	return (0, "ERROR: missing generated machine IR dump (.mir)", undef) if !defined($my_raw);

	my $ref_mir = canonicalize_machine_ir($ref_raw);
	my $my_mir = canonicalize_machine_ir($my_raw);
	return (1, undef, undef) if $ref_mir eq $my_mir;

	putrawdata("$my.cmir", $my_mir);
	my $hint = "To compare structural machine IR oracles:\n\n    \$ diff " .
		rooted_path("$ref.cmir") . " " . rooted_path("$my.cmir") . "\n\n";
	return (0, "ERROR: structural machine IR dumps do not match after canonicalization", $hint);
}

sub parse_link_map
{
	my ($file) = @_;
	open(my $fh, '<', $file) or die "Unable to open $file\n";
	my @lines;
	while (my $line = <$fh>)
	{
		chomp($line);
		next if $line =~ /^\s*$/;
		push @lines, $line;
	}
	close($fh);

	die "Invalid link map $file\n" if scalar(@lines) < 4;

	my %map = (
		objects => [],
		symbols => [],
	);

	if ($lines[0] !~ /^link_map x86_64 (\S+)$/)
	{
		die "Invalid link_map header in $file\n";
	}
	$map{target} = $1;

	if ($lines[1] !~ /^startup_size (\d+)$/)
	{
		die "Invalid startup_size in $file\n";
	}
	$map{startup_size} = 0 + $1;

	if ($lines[2] !~ /^code_size (\d+)$/)
	{
		die "Invalid code_size in $file\n";
	}
	$map{code_size} = 0 + $1;

	if ($lines[3] !~ /^data_size (\d+)$/)
	{
		die "Invalid data_size in $file\n";
	}
	$map{data_size} = 0 + $1;

	for (my $i = 4; $i < scalar(@lines); ++$i)
	{
		my $line = $lines[$i];
		if ($line =~ /^object (\d+) code_offset (\d+) data_offset (\d+)$/)
		{
			push @{$map{objects}}, {
				index => 0 + $1,
				code_offset => 0 + $2,
				data_offset => 0 + $3,
			};
		}
		elsif ($line =~ /^symbol (\S+) (code|data) (\d+)$/)
		{
			push @{$map{symbols}}, {
				name => $1,
				section => $2,
				offset => 0 + $3,
			};
		}
		else
		{
			die "Invalid link-map line in $file: $line\n";
		}
	}

	return \%map;
}

sub validate_link_map
{
	my ($map, $file) = @_;

	die "Unsupported target in $file\n"
		if $map->{target} ne 'macos' &&
		   $map->{target} ne 'linux';

	die "Invalid sizes in $file\n"
		if !looks_like_number($map->{startup_size}) ||
		   !looks_like_number($map->{code_size}) ||
		   !looks_like_number($map->{data_size});

	die "Inconsistent size ordering in $file\n"
		if $map->{startup_size} > $map->{code_size} ||
		   $map->{code_size} > $map->{data_size};

	for (my $i = 0; $i < scalar(@{$map->{objects}}); ++$i)
	{
		my $object = $map->{objects}[$i];
		die "Object index ordering mismatch in $file\n"
			if $object->{index} != $i;
		die "Object code offset out of range in $file\n"
			if $object->{code_offset} < $map->{startup_size} ||
			   $object->{code_offset} > $map->{code_size};
		die "Object data offset out of range in $file\n"
			if $object->{data_offset} < $map->{code_size} ||
			   $object->{data_offset} > $map->{data_size};
	}

	for (my $i = 0; $i < scalar(@{$map->{symbols}}); ++$i)
	{
		my $symbol = $map->{symbols}[$i];
		if ($symbol->{section} eq 'code')
		{
			die "Code symbol offset out of range in $file\n"
				if $symbol->{offset} < $map->{startup_size} ||
				   $symbol->{offset} >= $map->{code_size};
		}
		elsif ($symbol->{section} eq 'data')
		{
			die "Data symbol offset out of range in $file\n"
				if $symbol->{offset} < $map->{code_size} ||
				   $symbol->{offset} >= $map->{data_size};
		}
		else
		{
			die "Invalid symbol section in $file\n";
		}
		if ($i > 0 && $map->{symbols}[$i - 1]{name} gt $symbol->{name})
		{
			die "Symbol ordering mismatch in $file\n";
		}
	}
}

sub compare_link_maps_structural
{
	my ($ref_file, $my_file) = @_;
	my $ref = parse_link_map($ref_file);
	my $my = parse_link_map($my_file);

	validate_link_map($ref, $ref_file);
	validate_link_map($my, $my_file);

	return 0 if scalar(@{$ref->{objects}}) != scalar(@{$my->{objects}});
	return 0 if scalar(@{$ref->{symbols}}) != scalar(@{$my->{symbols}});

	for (my $i = 0; $i < scalar(@{$ref->{objects}}); ++$i)
	{
		return 0 if $ref->{objects}[$i]{index} != $my->{objects}[$i]{index};
	}

	for (my $i = 0; $i < scalar(@{$ref->{symbols}}); ++$i)
	{
		return 0 if $ref->{symbols}[$i]{name} ne $my->{symbols}[$i]{name};
		return 0 if $ref->{symbols}[$i]{section} ne $my->{symbols}[$i]{section};
	}

	return 1;
}

sub append_keep_going_summary
{
	my ($repo_root, $cwd, $npass, $suite_total, $failed) = @_;
	if (open(my $fh, '>>', "$repo_root/.test_counts"))
	{
		print $fh "$npass $suite_total\n";
		close($fh);
	}
	if ($failed)
	{
		system('touch', "$cwd/.test_failed");
	}
}

sub compare_text
{
	my ($suffix_pattern, $ref_suffix, $my_suffix, $testbase, $optional_output) = @_;
	my $ref = "$testbase.$ref_suffix";
	my $my = "$testbase.$my_suffix";
	my $ref_status = getdata("$ref.exit_status");
	my $my_status = getdata("$my.exit_status");
	my @missing_status = ();
	push @missing_status, "$ref.exit_status" if !defined($ref_status);
	push @missing_status, "$my.exit_status" if !defined($my_status);
	return (0, "ERROR: missing exit status output (" . join(', ', @missing_status) . ")")
		if scalar(@missing_status) != 0;
	return (0, status_mismatch_message("tool", $ref_status, $my_status))
		if $ref_status ne $my_status;
	return (1, undef) if $ref_status ne 'EXIT_SUCCESS';
	my $ref_data = getdata($ref);
	my $my_data = getdata($my);
	$ref_data = '' if $optional_output && !defined($ref_data);
	$my_data = '' if $optional_output && !defined($my_data);
	my @missing_output = ();
	push @missing_output, $ref if !defined($ref_data);
	push @missing_output, $my if !defined($my_data);
	return (0, "ERROR: missing output file (" . join(', ', @missing_output) . ")")
		if scalar(@missing_output) != 0;
	return ($ref_data eq $my_data
		? (1, undef)
		: (0, "ERROR: checked output does not match reference"));
}

sub lowir_symbol_basename
{
	my ($symbol) = @_;
	my $base = $symbol;
	$base =~ s/__ov\d+$//;
	$base =~ s/^.*___//;
	$base =~ s/^.*__//;
	return $base;
}

sub lowir_symbol_is_allowed_external
{
	my ($symbol, $source_text, $for_addr) = @_;
	return 1 if $symbol eq '__cxa_pure_virtual';
	return 1 if $symbol =~ /^__builtin_/;
	return 0;
}

sub lowir_trim
{
	my ($text) = @_;
	return '' if !defined($text);
	$text =~ s/^\s+//;
	$text =~ s/\s+$//;
	return $text;
}

sub lowir_type_pattern
{
	return qr/(?:[A-Za-z0-9_]+|obj<\d+x\d+>)/;
}

sub parse_lowir_object_type
{
	my ($type) = @_;
	return undef if !defined($type);
	return undef if $type !~ /^obj<(\d+)x(\d+)>$/;
	return {
		size => 0 + $1,
		align => 0 + $2,
	};
}

sub parse_lowir_param_list
{
	my ($param_text) = @_;
	$param_text = lowir_trim($param_text);
	return (1, []) if $param_text eq '';

	my @raw_params;
	my $current = '';
	my $bracket_depth = 0;
	for my $ch (split(//, $param_text))
	{
		if ($ch eq '[')
		{
			++$bracket_depth;
			$current .= $ch;
			next;
		}
		if ($ch eq ']')
		{
			return (0, "unterminated parameter metadata")
				if $bracket_depth <= 0;
			--$bracket_depth;
			$current .= $ch;
			next;
		}
		if ($ch eq ',' && $bracket_depth == 0)
		{
			my $trimmed = lowir_trim($current);
			return (0, "invalid parameter syntax: $current")
				if $trimmed eq '';
			push @raw_params, $trimmed;
			$current = '';
			next;
		}
		$current .= $ch;
	}
	return (0, "unterminated parameter metadata")
		if $bracket_depth != 0;
	my $trimmed = lowir_trim($current);
	return (0, "invalid parameter syntax: $current")
		if $trimmed eq '';
	push @raw_params, $trimmed;

	my @params;
	my $type_pattern = lowir_type_pattern();
	for my $param (@raw_params)
	{
		my ($name, $type, $metadata_suffix) =
			($param =~ /^%([A-Za-z0-9_]+)\s*:\s*($type_pattern)((?:\s+\[[^\]]+\])?)$/);
		return (0, "invalid parameter syntax: $param")
			if !defined($name);
		my ($metadata_ok, $metadata_or_error) =
			parse_lowir_parameter_metadata_suffix($metadata_suffix);
		return (0, $metadata_or_error) if !$metadata_ok;
		my $pass = $metadata_or_error->{pass};
		my $capture = $metadata_or_error->{capture};
		my $access = $metadata_or_error->{access};
		my $alias = $metadata_or_error->{alias};
		return (0, "unknown parameter pass mode '$pass'")
			if $pass !~ /^(?:direct|indirect_result|by_address|reference|decay)$/;
		return (0, "parameter %$name with pass mode '$pass' must have type ptr")
			if $pass ne 'direct' && $type ne 'ptr';
		return (0, "parameter %$name with capture mode '$capture' must have type ptr")
			if $capture ne '' && $type ne 'ptr';
		return (0, "parameter %$name with access mode '$access' must have type ptr")
			if $access ne '' && $type ne 'ptr';
		return (0, "parameter %$name with alias mode '$alias' must have type ptr")
			if $alias ne '' && $type ne 'ptr';
		push @params, {
			name => $name,
			type => $type,
			pass => $pass,
			capture => $capture,
			access => $access,
			alias => $alias,
		};
	}
	return (1, \@params);
}

sub parse_lowir_parameter_metadata_suffix
{
	my ($suffix) = @_;
	my %metadata = (
		pass => 'direct',
		capture => '',
		access => '',
		alias => '',
	);
	return (1, \%metadata) if lowir_trim($suffix) eq '';
	return (0, "invalid parameter metadata syntax")
		if $suffix !~ /^\s+\[([^\]]+)\]$/;
	my %saw;
	for my $item (split(/\s*,\s*/, $1))
	{
		my ($key, $value) = ($item =~ /^([A-Za-z0-9_]+)\s*=\s*([A-Za-z0-9_]+)$/);
		return (0, "invalid parameter metadata item '$item'")
			if !defined($key);
		return (0, "duplicate parameter metadata key '$key'")
			if $saw{$key};
		if ($key eq 'pass')
		{
			return (0, "unknown parameter pass mode '$value'")
				if $value !~ /^(?:direct|indirect_result|by_address|reference|decay)$/;
			$metadata{pass} = $value;
		}
		elsif ($key eq 'capture')
		{
			return (0, "unknown parameter capture mode '$value'")
				if $value !~ /^(?:nocapture|maycapture)$/;
			$metadata{capture} = $value;
		}
		elsif ($key eq 'access')
		{
			return (0, "unknown parameter access mode '$value'")
				if $value !~ /^(?:none|read|write|readwrite)$/;
			$metadata{access} = $value;
		}
		elsif ($key eq 'alias')
		{
			return (0, "unknown parameter alias mode '$value'")
				if $value !~ /^(?:noalias)$/;
			$metadata{alias} = $value;
		}
		else
		{
			return (0, "unknown parameter metadata key '$key'");
		}
		$saw{$key} = 1;
	}
	return (1, \%metadata);
}

sub parse_lowir_symbol_metadata_suffix
{
	my ($suffix) = @_;
	my %metadata = (
		storage => '',
		role => '',
		linkage => '',
		binding => '',
		object => '',
		keep_alias => '',
		prefer_local => '',
		object_root => '',
	);
	my %saw;
	pos($suffix) = 0;
	while ($suffix =~ /\G\s+\[([^\]]+)\]/gc)
	{
		my $group = $1;
		for my $item (split(/\s*,\s*/, $group))
		{
			my ($key, $value) = ($item =~ /^([A-Za-z0-9_]+)\s*=\s*([^\],\s]+)$/);
			return (0, "invalid symbol metadata item '$item'")
				if !defined($key);
			return (0, "duplicate symbol metadata key '$key'")
				if $saw{$key};
			if ($key eq 'role')
			{
				$metadata{role} = $value;
			}
			elsif ($key eq 'storage')
			{
				return (0, "unknown global storage '$value'")
					if $value !~ /^(?:writable|readonly|thread_local)$/;
				$metadata{storage} = $value;
			}
			elsif ($key eq 'linkage')
			{
				return (0, "unknown language linkage '$value'")
					if $value !~ /^(?:c|cpp)$/;
				$metadata{linkage} = $value;
			}
			elsif ($key eq 'binding')
			{
				return (0, "unknown symbol binding '$value'")
					if $value !~ /^(?:internal|strong|weak)$/;
				$metadata{binding} = $value;
			}
			elsif ($key eq 'object')
			{
				$metadata{object} = $value;
			}
			elsif ($key eq 'keep_alias')
			{
				return (0, "unknown keep_alias mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{keep_alias} = $value;
			}
			elsif ($key eq 'prefer_local')
			{
				return (0, "unknown prefer_local mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{prefer_local} = $value;
			}
			elsif ($key eq 'object_root')
			{
				return (0, "unknown object_root mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{object_root} = $value;
			}
			else
			{
				return (0, "unknown symbol metadata key '$key'");
			}
			$saw{$key} = 1;
		}
	}
	my $rest = substr($suffix, pos($suffix) || 0);
	return (0, "invalid trailing symbol metadata '$rest'")
		if lowir_trim($rest) ne '';
	return (1, \%metadata);
}

sub parse_lowir_function_metadata_suffix
{
	my ($suffix) = @_;
	my %metadata = (
		arity => 'fixed',
		effects => '',
		unwind => '',
		return => '',
		role => '',
		linkage => '',
		binding => '',
		object => '',
		tls_for => '',
		keep_alias => '',
		prefer_local => '',
		object_root => '',
		trivial_lifecycle => '',
	);
	my %saw;
	pos($suffix) = 0;
	while ($suffix =~ /\G\s+\[([^\]]+)\]/gc)
	{
		my $group = $1;
		for my $item (split(/\s*,\s*/, $group))
		{
			my ($key, $value) = ($item =~ /^([A-Za-z0-9_]+)\s*=\s*([^\],\s]+)$/);
			return (0, "invalid function metadata item '$item'")
				if !defined($key);
			return (0, "duplicate function metadata key '$key'")
				if $saw{$key};
			if ($key eq 'arity')
			{
				return (0, "unknown function arity mode '$value'")
					if $value !~ /^(?:fixed|variadic|prototype_relaxed)$/;
				$metadata{arity} = $value;
			}
			elsif ($key eq 'effects')
			{
				return (0, "unknown function effects mode '$value'")
					if $value !~ /^(?:readnone|readonly|readwrite)$/;
				$metadata{effects} = $value;
			}
			elsif ($key eq 'unwind')
			{
				return (0, "unknown function unwind mode '$value'")
					if $value !~ /^(?:no|may)$/;
				$metadata{unwind} = $value;
			}
			elsif ($key eq 'return')
			{
				return (0, "unknown function return mode '$value'")
					if $value !~ /^(?:noreturn|returns)$/;
				$metadata{return} = $value;
			}
			elsif ($key eq 'role')
			{
				$metadata{role} = $value;
			}
			elsif ($key eq 'linkage')
			{
				return (0, "unknown language linkage '$value'")
					if $value !~ /^(?:c|cpp)$/;
				$metadata{linkage} = $value;
			}
			elsif ($key eq 'binding')
			{
				return (0, "unknown symbol binding '$value'")
					if $value !~ /^(?:internal|strong|weak)$/;
				$metadata{binding} = $value;
			}
			elsif ($key eq 'object')
			{
				$metadata{object} = $value;
			}
			elsif ($key eq 'tls_for')
			{
				return (0, "invalid tls_for target '$value'")
					if $value !~ /^@[A-Za-z0-9_]+$/;
				$metadata{tls_for} = $value;
			}
			elsif ($key eq 'keep_alias')
			{
				return (0, "unknown keep_alias mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{keep_alias} = $value;
			}
			elsif ($key eq 'prefer_local')
			{
				return (0, "unknown prefer_local mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{prefer_local} = $value;
			}
			elsif ($key eq 'object_root')
			{
				return (0, "unknown object_root mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{object_root} = $value;
			}
			elsif ($key eq 'trivial_lifecycle')
			{
				return (0, "unknown trivial_lifecycle mode '$value'")
					if $value !~ /^(?:yes|no)$/;
				$metadata{trivial_lifecycle} = $value;
			}
			else
			{
				return (0, "unknown function metadata key '$key'");
			}
			$saw{$key} = 1;
		}
	}
	my $rest = substr($suffix, pos($suffix) || 0);
	return (0, "invalid trailing function metadata '$rest'")
		if lowir_trim($rest) ne '';
	return (1, \%metadata);
}

sub parse_lowir_call_signature_suffix
{
	my ($suffix) = @_;
	my %signature = (
		has_signature => 0,
		ret => undef,
		params => [],
		arity => 'fixed',
	);
	return (1, \%signature) if lowir_trim($suffix) eq '';
	my $type_pattern = lowir_type_pattern();
	return (0, "invalid call signature syntax")
		if $suffix !~ /^\s+as\s+\((.*?)\)\s*->\s*($type_pattern)((?:\s+\[[^\]]+\])*)$/;
	my ($params_text, $ret_type, $metadata_suffix) = ($1, $2, $3);
	my ($params_ok, $params_or_error) = parse_lowir_param_list($params_text);
	return (0, $params_or_error) if !$params_ok;
	my ($metadata_ok, $metadata_or_error) =
		parse_lowir_function_metadata_suffix($metadata_suffix);
	return (0, $metadata_or_error) if !$metadata_ok;
	return (0, "call signature metadata does not allow symbol metadata")
		if $metadata_or_error->{role} ne '' ||
		   $metadata_or_error->{linkage} ne '' ||
		   $metadata_or_error->{binding} ne '' ||
		   $metadata_or_error->{object} ne '' ||
		   $metadata_or_error->{tls_for} ne '' ||
		   $metadata_or_error->{keep_alias} ne '' ||
		   $metadata_or_error->{prefer_local} ne '' ||
		   $metadata_or_error->{object_root} ne '' ||
		   $metadata_or_error->{trivial_lifecycle} ne '';
	$signature{has_signature} = 1;
	$signature{ret} = $ret_type;
	$signature{params} = $params_or_error;
	$signature{arity} = $metadata_or_error->{arity};
	return (1, \%signature);
}

sub split_lowir_args
{
	my ($arg_text) = @_;
	$arg_text = lowir_trim($arg_text);
	return () if $arg_text eq '';
	return map { lowir_trim($_) } split(/\s*,\s*/, $arg_text);
}

sub validate_lowir_operand
{
	my ($operand, $state, $errors, $context) = @_;
	return if !defined($operand) || $operand eq '';
	return if $operand =~ /^-?(?:0x[0-9A-Fa-f]+|\d+)(?:[uUlL]*)$/;
	return if $operand =~ /^-?(?:\d+\.\d*|\d*\.\d+|\d+)(?:[eE][+-]?\d+)?[fFlL]?$/;
	return if $operand eq 'zero';
	if ($operand =~ /^%([A-Za-z0-9_]+)$/)
	{
		push @$errors, "$context uses undefined temporary %$1"
			if !exists($state->{temps}{$1});
		return;
	}
	if ($operand =~ /^\$([A-Za-z0-9_]+)$/)
	{
		push @$errors, "$context uses undefined slot \$$1"
			if !exists($state->{slots}{$1});
		return;
	}
	if ($operand =~ /^\^([A-Za-z0-9_]+)$/)
	{
		push @{$state->{block_targets}}, $1;
		return;
	}
	if ($operand =~ /^\@([A-Za-z0-9_]+)$/)
	{
		my $name = $1;
		return if exists($state->{all_symbols}{$name});
		return if lowir_symbol_is_allowed_external($name, $state->{source_text}, 1);
		push @$errors, "$context references undefined symbol \@$name";
		return;
	}
}

sub validate_lowir_pointer_operand
{
	my ($operand, $state, $errors, $context) = @_;
	validate_lowir_operand($operand, $state, $errors, $context);
	if ($operand =~ /^%([A-Za-z0-9_]+)$/ && exists($state->{temps}{$1}))
	{
		my $type = $state->{temps}{$1};
		push @$errors, "$context expects pointer-valued operand, got %$1 : $type"
			if defined($type) && $type ne 'ptr';
	}
}

sub validate_lowir_value_operand_type
{
	my ($operand, $expected_type, $state, $errors, $context) = @_;
	validate_lowir_operand($operand, $state, $errors, $context);
}

sub validate_lowir_copyobj_source_operand
{
	my ($byte_count, $byte_alignment, $operand, $state, $errors, $context) = @_;
	validate_lowir_operand($operand, $state, $errors, $context);
	if ($operand =~ /^%([A-Za-z0-9_]+)$/ && exists($state->{temps}{$1}))
	{
		my $type = $state->{temps}{$1};
		return if defined($type) && $type eq 'ptr';
		my $object = parse_lowir_object_type($type);
		if (defined($object))
		{
			push @$errors, "$context object-valued source size $object->{size} does not match copyobj byte count $byte_count"
				if $object->{size} != $byte_count;
			push @$errors, "$context object-valued source alignment $object->{align} does not match copyobj alignment $byte_alignment"
				if $object->{align} != $byte_alignment;
			return;
		}
		push @$errors, "$context expects pointer-valued or object-valued source operand, got %$1 : $type";
	}
}

sub parse_lowir_storage_span
{
	my ($text) = @_;
	return undef if $text !~ /^(\d+)(?:x(\d+))?$/;
	my $size = 0 + $1;
	my $alignment = defined($2) ? (0 + $2) : 1;
	return undef if $size <= 0 || $alignment <= 0 || ($alignment & ($alignment - 1)) != 0;
	return {
		size => $size,
		alignment => $alignment,
	};
}

sub validate_lowir_storage_operand
{
	my ($storage, $expected_type, $state, $errors, $context) = @_;
	if ($storage =~ /^\$([A-Za-z0-9_]+)$/)
	{
		my $name = $1;
		if (!exists($state->{slots}{$name}))
		{
			push @$errors, "$context uses undefined slot \$$name";
			return;
		}
		return;
	}
	if ($storage =~ /^\@([A-Za-z0-9_]+)$/)
	{
		my $name = $1;
		if (!exists($state->{global_symbols}{$name}) &&
		    !lowir_symbol_is_allowed_external($name, $state->{source_text}, 1))
		{
			push @$errors, "$context uses undefined global \@$name";
			return;
		}
		return;
	}
	validate_lowir_pointer_operand($storage, $state, $errors, $context);
}

sub validate_lowir_instruction
{
	my ($line, $state, $errors) = @_;
	my $func = $state->{name};
	my $block = $state->{current_block};
	my $context = "function \@$func block ^$block";
	my $type_pattern = lowir_type_pattern();

	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*const\s+([A-Za-z0-9_]+)\s+(.+)$/)
	{
		$state->{temps}{$1} = $2;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*copy\s+([A-Za-z0-9_]+)\s+(.+)$/)
	{
		validate_lowir_value_operand_type($3, $2, $state, $errors, "$context copy");
		$state->{temps}{$1} = $2;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*addr\s+(@[A-Za-z0-9_]+|\$[A-Za-z0-9_]+)$/)
	{
		validate_lowir_operand($2, $state, $errors, "$context addr");
		$state->{temps}{$1} = 'ptr';
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*load\s+([A-Za-z0-9_]+)\s+(.+)$/)
	{
		validate_lowir_storage_operand($3, $2, $state, $errors, "$context load");
		$state->{temps}{$1} = $2;
		return 0;
	}
	if ($line =~ /^store\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+)$/)
	{
		validate_lowir_value_operand_type($2, $1, $state, $errors, "$context store");
		validate_lowir_storage_operand($3, $1, $state, $errors, "$context store");
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*atomic_load\s+([A-Za-z0-9_]+)\s+(.+?),\s+(\d+)$/)
	{
		validate_lowir_pointer_operand($3, $state, $errors, "$context atomic_load");
		$state->{temps}{$1} = $2;
		return 0;
	}
	if ($line =~ /^atomic_store\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+?),\s+(\d+)$/)
	{
		validate_lowir_value_operand_type($2, $1, $state, $errors, "$context atomic_store");
		validate_lowir_pointer_operand($3, $state, $errors, "$context atomic_store");
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*atomic_exchange\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+?),\s+(\d+)$/)
	{
		validate_lowir_pointer_operand($3, $state, $errors, "$context atomic_exchange");
		validate_lowir_value_operand_type($4, $2, $state, $errors, "$context atomic_exchange");
		$state->{temps}{$1} = $2;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*atomic_compare_exchange\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+?),\s+(.+?),\s+(\d+),\s+(\d+)$/)
	{
		validate_lowir_pointer_operand($3, $state, $errors, "$context atomic_compare_exchange");
		validate_lowir_pointer_operand($4, $state, $errors, "$context atomic_compare_exchange");
		validate_lowir_value_operand_type($5, $2, $state, $errors, "$context atomic_compare_exchange");
		$state->{temps}{$1} = undef;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*index\s+([A-Za-z0-9_]+)(?:\s+\[projection=([A-Za-z0-9_]+)\])?\s+(.+?),\s+(.+)$/)
	{
		if (defined($3) &&
		    $3 ne 'array_element' &&
		    $3 ne 'field' &&
		    $3 ne 'base_subobject' &&
		    $3 ne 'reference_field')
		{
			push @$errors, "$context index has unknown projection '$3'";
		}
		validate_lowir_pointer_operand($4, $state, $errors, "$context index");
		validate_lowir_operand($5, $state, $errors, "$context index");
		$state->{temps}{$1} = 'ptr';
		return 0;
	}
	if ($line =~ /^copyobj\s+([0-9]+(?:x[0-9]+)?)\s+(.+?),\s+(.+)$/)
	{
		my $span = parse_lowir_storage_span($1);
		push @$errors, "$context copyobj must use positive byte count and power-of-two alignment"
			if !defined($span);
		validate_lowir_copyobj_source_operand($span->{size}, $span->{alignment}, $2, $state, $errors, "$context copyobj")
			if defined($span);
		validate_lowir_pointer_operand($3, $state, $errors, "$context copyobj");
		return 0;
	}
	if ($line =~ /^zeroinit\s+([0-9]+(?:x[0-9]+)?)\s+(.+)$/)
	{
		my $span = parse_lowir_storage_span($1);
		push @$errors, "$context zeroinit must use positive byte count and power-of-two alignment"
			if !defined($span);
		validate_lowir_pointer_operand($2, $state, $errors, "$context zeroinit");
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*unary\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s+(.+)$/)
	{
		validate_lowir_value_operand_type($4, $3, $state, $errors, "$context unary");
		$state->{temps}{$1} = $3;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*binary\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+)$/)
	{
		validate_lowir_value_operand_type($4, $3, $state, $errors, "$context binary");
		validate_lowir_value_operand_type($5, $3, $state, $errors, "$context binary");
		$state->{temps}{$1} = $3;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*cmp\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+)$/)
	{
		validate_lowir_value_operand_type($4, $3, $state, $errors, "$context cmp");
		validate_lowir_value_operand_type($5, $3, $state, $errors, "$context cmp");
		$state->{temps}{$1} = undef;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*convert\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s+(.+)$/)
	{
		validate_lowir_value_operand_type($5, $4, $state, $errors, "$context convert");
		$state->{temps}{$1} = $3;
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*atomic_add_fetch\s+([A-Za-z0-9_]+)\s+(.+?),\s+(.+?),\s+(\d+)$/)
	{
		validate_lowir_pointer_operand($3, $state, $errors, "$context atomic_add_fetch");
		validate_lowir_value_operand_type($4, $2, $state, $errors, "$context atomic_add_fetch");
		$state->{temps}{$1} = $2;
		return 0;
	}
	if ($line =~ /^atomic_thread_fence\s+(\d+)$/ || $line =~ /^atomic_signal_fence\s+(\d+)$/)
	{
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*call\s+($type_pattern)\s+@([A-Za-z0-9_]+)\((.*?)\)(.*)$/)
	{
		my ($dst, $ret_type, $callee, $arg_text, $suffix) = ($1, $2, $3, $4, $5);
		my @args = split_lowir_args($arg_text);
		validate_lowir_operand($_, $state, $errors, "$context call \@$callee") for @args;
		my ($sig_ok, $call_sig) = parse_lowir_call_signature_suffix($suffix);
		push @$errors, "$context call \@$callee has invalid signature metadata: $call_sig"
			if !$sig_ok;
		if (exists($state->{function_symbols}{$callee}))
		{
			my $sig = $state->{signatures}{$callee};
			push @$errors, "$context call \@$callee expects return type $sig->{ret}, got $ret_type"
				if $sig->{ret} ne $ret_type;
			if ($sig->{arity} eq 'variadic' || $sig->{arity} eq 'prototype_relaxed')
			{
				push @$errors, "$context call \@$callee expects at least " .
					scalar(@{$sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) < scalar(@{$sig->{params}});
			}
			else
			{
				push @$errors, "$context call \@$callee expects exactly " .
					scalar(@{$sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) != scalar(@{$sig->{params}});
			}
			if ($sig_ok && $call_sig->{has_signature})
			{
				push @$errors, "$context direct call \@$callee has mismatched explicit signature return type $call_sig->{ret}"
					if $call_sig->{ret} ne $sig->{ret};
				push @$errors, "$context direct call \@$callee has mismatched explicit signature arity $call_sig->{arity}"
					if $call_sig->{arity} ne $sig->{arity};
				push @$errors, "$context direct call \@$callee has mismatched explicit signature parameter count " .
					scalar(@{$call_sig->{params}}) . " (expected " . scalar(@{$sig->{params}}) . ")"
					if scalar(@{$call_sig->{params}}) != scalar(@{$sig->{params}});
			}
		}
		elsif (exists($state->{global_symbols}{$callee}))
		{
			push @$errors, "$context indirect global call \@$callee requires explicit call signature"
				if !$sig_ok || !$call_sig->{has_signature};
			if ($sig_ok && $call_sig->{has_signature})
			{
				push @$errors, "$context indirect global call \@$callee signature return type $call_sig->{ret} does not match call result type $ret_type"
					if $call_sig->{ret} ne $ret_type;
				if ($call_sig->{arity} eq 'variadic' || $call_sig->{arity} eq 'prototype_relaxed')
				{
					push @$errors, "$context indirect global call \@$callee expects at least " .
						scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
						if scalar(@args) < scalar(@{$call_sig->{params}});
				}
				else
				{
					push @$errors, "$context indirect global call \@$callee expects exactly " .
						scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
						if scalar(@args) != scalar(@{$call_sig->{params}});
				}
			}
		}
		elsif (!lowir_symbol_is_allowed_external($callee, $state->{source_text}, 0))
		{
			push @$errors, "$context calls undefined function \@$callee";
		}
		$state->{temps}{$dst} = $ret_type;
		return 0;
	}
	if ($line =~ /^call\s+void\s+@([A-Za-z0-9_]+)\((.*?)\)(.*)$/)
	{
		my ($callee, $arg_text, $suffix) = ($1, $2, $3);
		my @args = split_lowir_args($arg_text);
		validate_lowir_operand($_, $state, $errors, "$context call \@$callee") for @args;
		my ($sig_ok, $call_sig) = parse_lowir_call_signature_suffix($suffix);
		push @$errors, "$context call \@$callee has invalid signature metadata: $call_sig"
			if !$sig_ok;
		if (exists($state->{function_symbols}{$callee}))
		{
			my $sig = $state->{signatures}{$callee};
			push @$errors, "$context call \@$callee expects return type $sig->{ret}, got void"
				if $sig->{ret} ne 'void';
			if ($sig->{arity} eq 'variadic' || $sig->{arity} eq 'prototype_relaxed')
			{
				push @$errors, "$context call \@$callee expects at least " .
					scalar(@{$sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) < scalar(@{$sig->{params}});
			}
			else
			{
				push @$errors, "$context call \@$callee expects exactly " .
					scalar(@{$sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) != scalar(@{$sig->{params}});
			}
			if ($sig_ok && $call_sig->{has_signature})
			{
				push @$errors, "$context direct call \@$callee has mismatched explicit signature return type $call_sig->{ret}"
					if $call_sig->{ret} ne 'void';
				push @$errors, "$context direct call \@$callee has mismatched explicit signature arity $call_sig->{arity}"
					if $call_sig->{arity} ne $sig->{arity};
				push @$errors, "$context direct call \@$callee has mismatched explicit signature parameter count " .
					scalar(@{$call_sig->{params}}) . " (expected " . scalar(@{$sig->{params}}) . ")"
					if scalar(@{$call_sig->{params}}) != scalar(@{$sig->{params}});
			}
		}
		elsif (exists($state->{global_symbols}{$callee}))
		{
			push @$errors, "$context indirect global call \@$callee requires explicit call signature"
				if !$sig_ok || !$call_sig->{has_signature};
			if ($sig_ok && $call_sig->{has_signature})
			{
				push @$errors, "$context indirect global call \@$callee signature return type $call_sig->{ret} does not match call result type void"
					if $call_sig->{ret} ne 'void';
				if ($call_sig->{arity} eq 'variadic' || $call_sig->{arity} eq 'prototype_relaxed')
				{
					push @$errors, "$context indirect global call \@$callee expects at least " .
						scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
						if scalar(@args) < scalar(@{$call_sig->{params}});
				}
				else
				{
					push @$errors, "$context indirect global call \@$callee expects exactly " .
						scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
						if scalar(@args) != scalar(@{$call_sig->{params}});
				}
			}
		}
		elsif (!lowir_symbol_is_allowed_external($callee, $state->{source_text}, 0))
		{
			push @$errors, "$context calls undefined function \@$callee";
		}
		return 0;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*call\s+($type_pattern)\s+(.+?)\((.*?)\)(.*)$/)
	{
		my ($dst, $ret_type, $callee, $arg_text, $suffix) = ($1, $2, $3, $4, $5);
		validate_lowir_pointer_operand($callee, $state, $errors, "$context indirect call");
		my @args = split_lowir_args($arg_text);
		validate_lowir_operand($_, $state, $errors, "$context indirect call") for @args;
		my ($sig_ok, $call_sig) = parse_lowir_call_signature_suffix($suffix);
		push @$errors, "$context indirect call has invalid signature metadata: $call_sig"
			if !$sig_ok;
		push @$errors, "$context indirect call requires explicit call signature"
			if !$sig_ok || !$call_sig->{has_signature};
		if ($sig_ok && $call_sig->{has_signature})
		{
			push @$errors, "$context indirect call signature return type $call_sig->{ret} does not match call result type $ret_type"
				if $call_sig->{ret} ne $ret_type;
			if ($call_sig->{arity} eq 'variadic' || $call_sig->{arity} eq 'prototype_relaxed')
			{
				push @$errors, "$context indirect call expects at least " .
					scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) < scalar(@{$call_sig->{params}});
			}
			else
			{
				push @$errors, "$context indirect call expects exactly " .
					scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) != scalar(@{$call_sig->{params}});
			}
		}
		$state->{temps}{$dst} = $ret_type;
		return 0;
	}
	if ($line =~ /^call\s+void\s+(.+?)\((.*?)\)(.*)$/)
	{
		validate_lowir_pointer_operand($1, $state, $errors, "$context indirect call");
		my @args = split_lowir_args($2);
		validate_lowir_operand($_, $state, $errors, "$context indirect call") for @args;
		my ($sig_ok, $call_sig) = parse_lowir_call_signature_suffix($3);
		push @$errors, "$context indirect call has invalid signature metadata: $call_sig"
			if !$sig_ok;
		push @$errors, "$context indirect call requires explicit call signature"
			if !$sig_ok || !$call_sig->{has_signature};
		if ($sig_ok && $call_sig->{has_signature})
		{
			push @$errors, "$context indirect call signature return type $call_sig->{ret} does not match call result type void"
				if $call_sig->{ret} ne 'void';
			if ($call_sig->{arity} eq 'variadic' || $call_sig->{arity} eq 'prototype_relaxed')
			{
				push @$errors, "$context indirect call expects at least " .
					scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) < scalar(@{$call_sig->{params}});
			}
			else
			{
				push @$errors, "$context indirect call expects exactly " .
					scalar(@{$call_sig->{params}}) . " argument(s), got " . scalar(@args)
					if scalar(@args) != scalar(@{$call_sig->{params}});
			}
		}
		return 0;
	}
	if ($line =~ /^jump\s+(\^[A-Za-z0-9_]+)$/)
	{
		validate_lowir_operand($1, $state, $errors, "$context jump");
		return 1;
	}
	if ($line =~ /^branch\s+(.+?),\s+(\^[A-Za-z0-9_]+),\s+(\^[A-Za-z0-9_]+)$/)
	{
		validate_lowir_operand($1, $state, $errors, "$context branch");
		validate_lowir_operand($2, $state, $errors, "$context branch");
		validate_lowir_operand($3, $state, $errors, "$context branch");
		return 1;
	}
	if ($line =~ /^switch\s+(.+?),\s+(\^[A-Za-z0-9_]+)(.*)$/)
	{
		my ($selector, $default_target, $rest) = ($1, $2, $3);
		validate_lowir_operand($selector, $state, $errors, "$context switch");
		validate_lowir_operand($default_target, $state, $errors, "$context switch");
		$rest = lowir_trim($rest);
		if ($rest ne '')
		{
			if ($rest !~ s/^,\s*//)
			{
				push @$errors, "$context switch has invalid case list: $rest";
				return 1;
			}
			for my $case (split(/\s*,\s*/, $rest))
			{
				if ($case !~ /^(.+):(\^[A-Za-z0-9_]+)$/)
				{
					push @$errors, "$context switch has invalid case arm: $case";
					next;
				}
				validate_lowir_operand($1, $state, $errors, "$context switch");
				validate_lowir_operand($2, $state, $errors, "$context switch");
			}
		}
		return 1;
	}
	if ($line =~ /^return\s+void$/)
	{
		push @$errors, "$context returns void from function returning $state->{ret}"
			if $state->{ret} ne 'void';
		return 1;
	}
	if ($line =~ /^return\s+($type_pattern)\s+(.+)$/)
	{
		validate_lowir_value_operand_type($2, $1, $state, $errors, "$context return");
		push @$errors, "$context returns $1 from function returning $state->{ret}"
			if $state->{ret} ne $1;
		return 1;
	}
	if ($line =~ /^eh_try\s+(\^[A-Za-z0-9_]+)$/ || $line =~ /^eh_cleanup\s+(\^[A-Za-z0-9_]+)$/)
	{
		validate_lowir_operand($1, $state, $errors, "$context handler setup");
		return 0;
	}
	if ($line =~ /^eh_end$/)
	{
		return 0;
	}
	if ($line =~ /^throw\s+($type_pattern)\s+(.+)$/)
	{
		validate_lowir_value_operand_type($2, $1, $state, $errors, "$context throw");
		return 1;
	}
	if ($line =~ /^resume$/)
	{
		return 1;
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=\s*exception\s+($type_pattern)$/)
	{
		$state->{temps}{$1} = $2;
		return 0;
	}

	my $rhs = $line;
	$rhs =~ s/^%[A-Za-z0-9_]+\s*=\s*//;
	for my $operand ($rhs =~ /(%[A-Za-z0-9_]+|\$[A-Za-z0-9_]+|\^[A-Za-z0-9_]+|@[A-Za-z0-9_]+)/g)
	{
		validate_lowir_operand($operand, $state, $errors, "$context instruction");
	}
	if ($line =~ /^%([A-Za-z0-9_]+)\s*=/)
	{
		$state->{temps}{$1} = undef;
	}
	return 0;
}

sub validate_lowir_structure
{
	my ($data, $source_text, $all_symbols, $function_symbols, $global_symbols, $global_types, $signatures) = @_;
	my @errors;
	my @lines = split(/\n/, $data);
	my $type_pattern = lowir_type_pattern();

	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		my $line = $lines[$i];
		next if $line =~ /^\s*$/;

		if ($line =~ /^global @([A-Za-z0-9_]+)(?:\s+readonly)?((?:\s+\[[^\]]+\])*)\s*=\s*\{$/)
		{
			++$i while $i + 1 < scalar(@lines) && $lines[$i + 1] !~ /^\}$/;
			next;
		}
		next if $line =~ /^declare global @([A-Za-z0-9_]+)(?:\s*:\s*$type_pattern)?((?:\s+\[[^\]]+\])*)$/;
		next if $line =~ /^global @([A-Za-z0-9_]+)(?:\s+readonly)?\s*:\s*$type_pattern((?:\s+\[[^\]]+\])*)\s*=/;

		next if $line !~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/;
		my ($name, $param_text, $ret_type, $metadata_suffix) = ($1, $2, $3, $4);
		my ($params_ok, $params_or_error) = parse_lowir_param_list($param_text);
		if (!$params_ok)
		{
			push @errors, "function \@$name has invalid parameter list: $params_or_error";
			next;
		}
		my ($metadata_ok, $metadata_or_error) =
			parse_lowir_function_metadata_suffix($metadata_suffix);
		if (!$metadata_ok)
		{
			push @errors, "function \@$name has invalid metadata: $metadata_or_error";
			next;
		}
		for (my $pi = 0; $pi < scalar(@$params_or_error); ++$pi)
		{
			next if $params_or_error->[$pi]{pass} ne 'indirect_result';
			push @errors, "function \@$name indirect result parameter must appear first"
				if $pi != 0;
			push @errors, "function \@$name indirect result parameter requires return type void"
				if $ret_type ne 'void';
		}

		my %temps = map { $_->{name} => $_->{type} } @$params_or_error;
		my %slots;
		my %blocks;
		my %block_has_terminator;
		my @block_targets;
		my $current_block;
		my $saw_block = 0;

		++$i;
		for (; $i < scalar(@lines); ++$i)
		{
			my $body = $lines[$i];
			last if $body =~ /^\}$/;
			next if $body =~ /^\s*$/;

			if ($body =~ /^\s*slot \$([A-Za-z0-9_]+)\s*:\s*($type_pattern)$/)
			{
				push @errors, "function \@$name declares slot after blocks started"
					if $saw_block;
				push @errors, "function \@$name has duplicate slot \$$1"
					if exists($slots{$1});
				$slots{$1} = $2;
				next;
			}

			if ($body =~ /^\s*block \^([A-Za-z0-9_]+):$/)
			{
				push @errors, "function \@$name block ^$current_block is missing terminator"
					if defined($current_block) && !$block_has_terminator{$current_block};
				push @errors, "function \@$name has duplicate block ^$1"
					if exists($blocks{$1});
				$blocks{$1} = 1;
				$block_has_terminator{$1} = 0;
				$current_block = $1;
				$saw_block = 1;
				next;
			}

			if (!defined($current_block))
			{
				push @errors, "function \@$name has instruction outside of any block: $body";
				next;
			}

			if ($block_has_terminator{$current_block})
			{
				push @errors, "function \@$name block ^$current_block has instruction after terminator: $body";
				next;
			}

			my %state = (
				name => $name,
				ret => $ret_type,
				current_block => $current_block,
				temps => \%temps,
				slots => \%slots,
				block_targets => \@block_targets,
				all_symbols => $all_symbols,
				function_symbols => $function_symbols,
				global_symbols => $global_symbols,
				global_types => $global_types,
				signatures => $signatures,
				source_text => $source_text,
			);
			$block_has_terminator{$current_block} =
				validate_lowir_instruction(lowir_trim($body), \%state, \@errors);
		}

		push @errors, "function \@$name contains no blocks"
			if !$saw_block;
		push @errors, "function \@$name block ^$current_block is missing terminator"
			if defined($current_block) && !$block_has_terminator{$current_block};

		my %missing_blocks;
		for my $target (@block_targets)
		{
			$missing_blocks{$target} = 1 if !exists($blocks{$target});
		}
		push @errors, "function \@$name references undefined block target(s): " .
			join(', ', map { "^$_" } sort keys(%missing_blocks))
			if scalar(keys(%missing_blocks)) > 0;
	}

	return @errors;
}

sub lowir_top_level_order_kind
{
	my ($line) = @_;
	return ('declare global', 0, $1)
		if $line =~ /^declare global @([A-Za-z0-9_]+)\b/;
	return ('declare function', 1, $1)
		if $line =~ /^declare function @([A-Za-z0-9_]+)\b/;
	return ('global', 2, $1)
		if $line =~ /^global @([A-Za-z0-9_]+)\b/;
	return ('function', 3, $1)
		if $line =~ /^function @([A-Za-z0-9_]+)\b/;
	return ('alias object', 4, $1)
		if $line =~ /^alias object ([^\s=]+)\s*=/;
	return undef;
}

sub validate_lowir_top_level_order
{
	my ($data) = @_;
	my @errors;
	my @lines = split(/\n/, $data);
	my $last_rank = -1;
	my $last_kind = 'start of file';
	my $last_symbol = '';
	my $expected =
		'declare global, declare function, global, function, alias object';

	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		my ($kind, $rank, $symbol) = lowir_top_level_order_kind($lines[$i]);
		next if !defined($kind);
		if ($rank < $last_rank)
		{
			my $previous = $last_kind;
			$previous .= " \@$last_symbol" if $last_symbol ne '';
			push @errors, "top-level LowIR order violation at line " . ($i + 1) .
				": $kind \@$symbol appears after $previous; expected order is $expected";
		}
		$last_rank = $rank if $rank > $last_rank;
		$last_kind = $kind;
		$last_symbol = $symbol;
	}

	return @errors;
}

sub lowir_special_member_order_keys
{
	my ($object_symbol) = @_;
	return () if !defined($object_symbol) || $object_symbol eq '';

	if ($object_symbol =~ /^(.*)C([12])([^ ]*)$/)
	{
		my ($owner, $entry, $suffix) = ($1, $2, $3);
		my @keys;
		my $constructor_kind = '';
		if ($suffix =~ /^ERK/)
		{
			$constructor_kind = 'copy constructor';
			push @keys, {
				group => "$owner constructor copy-move",
				rank => 0,
				description => 'copy constructor',
				expectation => 'copy constructors must precede move constructors',
			};
		}
		elsif ($suffix =~ /^EO/)
		{
			$constructor_kind = 'move constructor';
			push @keys, {
				group => "$owner constructor copy-move",
				rank => 1,
				description => 'move constructor',
				expectation => 'copy constructors must precede move constructors',
			};
		}
		else
		{
			$constructor_kind = 'constructor';
		}
		my $entry_rank = $entry eq '2' ? 0 : 1;
		my $entry_description = $entry eq '2' ? 'base entry' : 'complete entry';
		push @keys, {
			group => "$owner constructor entry $suffix",
			rank => $entry_rank,
			description => "$constructor_kind $entry_description",
			expectation => 'constructor base entries must precede complete entries',
		};
		return @keys;
	}

	if ($object_symbol =~ /^(.*)aS([^ ]*)$/)
	{
		my ($owner, $suffix) = ($1, $2);
		if ($suffix =~ /^ERK/)
		{
			return {
				group => "$owner assignment copy-move",
				rank => 30,
				description => 'copy assignment',
				expectation => 'copy assignments must precede move assignments',
			};
		}
		if ($suffix =~ /^EO/)
		{
			return {
				group => "$owner assignment copy-move",
				rank => 40,
				description => 'move assignment',
				expectation => 'copy assignments must precede move assignments',
			};
		}
		return ();
	}

	if ($object_symbol =~ /^(.*)D([012])(?:Ev|E.*)$/)
	{
		my ($owner, $entry) = ($1, $2);
		my %rank = (
			2 => 50,
			0 => 51,
			1 => 52,
		);
		my %description = (
			2 => 'destructor base entry',
			0 => 'destructor deleting entry',
			1 => 'destructor complete entry',
		);
		return {
			group => "$owner destructor entry",
			rank => $rank{$entry},
			description => $description{$entry},
			expectation => 'destructor entries must be ordered base, deleting, complete',
		};
	}

	return ();
}

sub validate_lowir_special_member_order
{
	my ($data) = @_;
	my @errors;
	my %last_by_group;
	my $type_pattern = lowir_type_pattern();
	my @lines = split(/\n/, $data);

	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		my $line = $lines[$i];
		next if $line !~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/;
		my ($symbol, $metadata_suffix) = ($1, $4);
		my ($ok, $metadata_or_error) = parse_lowir_function_metadata_suffix($metadata_suffix);
		next if !$ok;
		my @keys = lowir_special_member_order_keys($metadata_or_error->{object});
		for my $key (@keys)
		{
			my $group = $key->{group};
			if (exists($last_by_group{$group}) &&
			    $key->{rank} < $last_by_group{$group}{rank})
			{
				push @errors, "special member LowIR order violation at line " . ($i + 1) .
					": function \@$symbol ($key->{description}) appears after " .
					"$last_by_group{$group}{description}; $key->{expectation}";
			}
			$last_by_group{$group} = {
				rank => $key->{rank},
				description => $key->{description},
			};
		}
	}

	return @errors;
}

sub lowir_destructor_entry_from_object_symbol
{
	my ($object_symbol) = @_;
	return undef if !defined($object_symbol) || $object_symbol eq '';
	return undef if $object_symbol !~ /^(.*)D([012])(?:Ev|E.*)$/;
	return {
		owner => $1,
		entry => $2,
	};
}

sub validate_lowir_vtable_destructor_slot_order
{
	my ($data) = @_;
	my @errors;
	my %function_object;
	my $type_pattern = lowir_type_pattern();
	my @lines = split(/\n/, $data);

	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		my $line = $lines[$i];
		next if $line !~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/;
		my ($symbol, $metadata_suffix) = ($1, $4);
		my ($ok, $metadata_or_error) = parse_lowir_function_metadata_suffix($metadata_suffix);
		next if !$ok;
		next if !exists($metadata_or_error->{object});
		$function_object{$symbol} = $metadata_or_error->{object};
	}

	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		next if $lines[$i] !~ /^global @([A-Za-z0-9_]+)\b.*=\s*\{$/;
		my $global = $1;
		next if $global !~ /vtable/;

		my @slots;
		for (++$i; $i < scalar(@lines); ++$i)
		{
			last if $lines[$i] =~ /^\}$/;
			next if $lines[$i] !~ /^\s*ptr addr @([A-Za-z0-9_]+)(?:\s|$)/;
			my $target = $1;
			my $object = $function_object{$target};
			my $entry = lowir_destructor_entry_from_object_symbol($object);
			push @slots, {
				symbol => $target,
				object => $object,
				owner => defined($entry) ? $entry->{owner} : '',
				entry => defined($entry) ? $entry->{entry} : '',
			};
		}

		for (my $slot = 0; $slot < scalar(@slots); ++$slot)
		{
			my $entry = $slots[$slot]{entry};
			next if $entry eq '';
			if ($entry eq '2')
			{
				push @errors, "vtable destructor slot order violation in \@$global: " .
					"function \@$slots[$slot]{symbol} is a base destructor entry; " .
					"vtable destructor slots must be complete, deleting";
				next;
			}
			if ($entry eq '1')
			{
				if ($slot + 1 >= scalar(@slots) ||
				    $slots[$slot + 1]{owner} ne $slots[$slot]{owner} ||
				    $slots[$slot + 1]{entry} ne '0')
				{
					push @errors, "vtable destructor slot order violation in \@$global: " .
						"function \@$slots[$slot]{symbol} (complete destructor entry) " .
						"must be immediately followed by the matching deleting destructor entry";
				}
				next;
			}
			if ($entry eq '0')
			{
				if ($slot == 0 ||
				    $slots[$slot - 1]{owner} ne $slots[$slot]{owner} ||
				    $slots[$slot - 1]{entry} ne '1')
				{
					push @errors, "vtable destructor slot order violation in \@$global: " .
						"function \@$slots[$slot]{symbol} (deleting destructor entry) " .
						"must appear immediately after the matching complete destructor entry";
				}
			}
		}
	}

	return @errors;
}

sub validate_lowir_function_role_order
{
	my ($data) = @_;
	my @errors;
	my $seen_fini = 0;
	my $fini_symbol = '';
	my $type_pattern = lowir_type_pattern();
	my @lines = split(/\n/, $data);

	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		my $line = $lines[$i];
		next if $line !~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/;
		my ($symbol, $metadata_suffix) = ($1, $4);
		my ($ok, $metadata_or_error) = parse_lowir_function_metadata_suffix($metadata_suffix);
		next if !$ok;
		next if !exists($metadata_or_error->{role});
		if ($metadata_or_error->{role} eq 'fini')
		{
			$seen_fini = 1;
			$fini_symbol = $symbol;
			next;
		}
		if ($metadata_or_error->{role} eq 'init' && $seen_fini)
		{
			push @errors, "LowIR role order violation at line " . ($i + 1) .
				": init function \@$symbol appears after fini function \@$fini_symbol";
		}
	}

	return @errors;
}

sub lowir_role_owner_kind
{
	my ($role) = @_;
	return '' if !defined($role) || $role eq '';
	return 'function' if $role =~ /^(?:entry|init|fini|eh_unhandled|eh_allocate_exception|eh_begin_catch|eh_call_unexpected|eh_current_exception_type|eh_end_catch|eh_rethrow|eh_throw|eh_personality|eh_resume)$/;
	return 'global' if $role =~ /^(?:eh_top|eh_value|eh_type)$/;
	return '';
}

sub validate_lowir_roles
{
	my ($data) = @_;
	my @errors;
	my %role_owner;
	my $type_pattern = lowir_type_pattern();

	for my $line (split(/\n/, $data))
	{
		next if $line =~ /^\s*$/;
		my ($symbol, $kind, $role);
		if ($line =~ /^declare global @([A-Za-z0-9_]+)(?:\s*:\s*$type_pattern)?((?:\s+\[[^\]]+\])*)$/)
		{
			my ($ok, $metadata_or_error) = parse_lowir_symbol_metadata_suffix($2);
			if (!$ok)
			{
				push @errors, "declare global \@$1 has invalid metadata: $metadata_or_error";
				next;
			}
			($symbol, $role) = ($1, $metadata_or_error->{role});
			$kind = 'global';
		}
		elsif ($line =~ /^global @([A-Za-z0-9_]+)(?:\s+readonly)?(?:\s*:\s*$type_pattern)?((?:\s+\[[^\]]+\])*)\s*=/)
		{
			my ($ok, $metadata_or_error) = parse_lowir_symbol_metadata_suffix($2);
			if (!$ok)
			{
				push @errors, "global \@$1 has invalid metadata: $metadata_or_error";
				next;
			}
			($symbol, $role) = ($1, $metadata_or_error->{role});
			$kind = 'global';
		}
		elsif ($line =~ /^declare function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*)$/)
		{
			my ($ok, $metadata_or_error) = parse_lowir_function_metadata_suffix($4);
			if (!$ok)
			{
				push @errors, "declare function \@$1 has invalid metadata: $metadata_or_error";
				next;
			}
			($symbol, $role) = ($1, $metadata_or_error->{role});
			$kind = 'function';
		}
		elsif ($line =~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/)
		{
			my ($ok, $metadata_or_error) = parse_lowir_function_metadata_suffix($4);
			if (!$ok)
			{
				push @errors, "function \@$1 has invalid metadata: $metadata_or_error";
				next;
			}
			($symbol, $role) = ($1, $metadata_or_error->{role});
			$kind = 'function';
		}
		next if !defined($role) || $role eq '';

		my $expected_kind = lowir_role_owner_kind($role);
		if ($expected_kind eq '')
		{
			push @errors, "unknown LowIR role '$role' on \@$symbol";
			next;
		}
		if ($expected_kind ne $kind)
		{
			push @errors, "LowIR role '$role' has invalid $kind owner \@$symbol";
			next;
		}
		if (exists($role_owner{$role}) && $role_owner{$role} ne $symbol)
		{
			push @errors, "duplicate LowIR role '$role': \@$role_owner{$role}, \@$symbol";
			next;
		}
		$role_owner{$role} = $symbol;
	}

	return @errors;
}

sub validate_lowir_text
{
	my ($data, $source_file, $options) = @_;
	$options = {} if !defined($options);
	return (1, undef) if !defined($data) || $data eq '';

	my $source_text = getrawdata($source_file);
	my @errors;

	my @tops = ($data =~ /^(?:declare\s+(?:function|global)|function|global)\s+@([A-Za-z0-9_]+)\b/gm);
	my %top_count;
	for my $name (@tops)
	{
		++$top_count{$name};
	}
	my @duplicates = sort grep { $top_count{$_} > 1 } keys(%top_count);
	push @errors, "duplicate LowIR symbol entries: " . join(', ', @duplicates) if scalar(@duplicates) > 0;

	my %alias_count;
	my @alias_targets;
	for my $line (split(/\n/, $data))
	{
		next if $line !~ /^alias object\b/;
		if ($line !~ /^alias object ([^\s=]+)\s*=\s*@([A-Za-z0-9_]+)\s*$/)
		{
			push @errors, "invalid object alias syntax: $line";
			next;
		}
		++$alias_count{$1};
		push @alias_targets, $2;
	}
	my @duplicate_aliases = sort grep { $alias_count{$_} > 1 } keys(%alias_count);
	push @errors, "duplicate LowIR object aliases: " . join(', ', @duplicate_aliases)
		if scalar(@duplicate_aliases) > 0;
	if ($options->{strict_presentation_order})
	{
		push @errors, validate_lowir_top_level_order($data);
		push @errors, validate_lowir_special_member_order($data);
		push @errors, validate_lowir_function_role_order($data);
	}
	push @errors, validate_lowir_vtable_destructor_slot_order($data);

	my %all_symbols;
	my %function_symbols;
	my %global_symbols;
	my %global_types;
	my $type_pattern = lowir_type_pattern();
	while ($data =~ /^global @([A-Za-z0-9_]+)(?:\s+readonly)?\s*:\s*($type_pattern)((?:\s+\[[^\]]+\])*)\s*=/gm)
	{
		$all_symbols{$1} = 1;
		$global_symbols{$1} = 1;
		$global_types{$1} = $2;
	}
	while ($data =~ /^global @([A-Za-z0-9_]+)(?:\s+readonly)?((?:\s+\[[^\]]+\])*)\s*=\s*\{$/gm)
	{
		$all_symbols{$1} = 1;
		$global_symbols{$1} = 1;
	}
	while ($data =~ /^declare global @([A-Za-z0-9_]+)\s*:\s*($type_pattern)((?:\s+\[[^\]]+\])*)$/gm)
	{
		$all_symbols{$1} = 1;
		$global_symbols{$1} = 1;
		$global_types{$1} = $2;
	}
	while ($data =~ /^declare global @([A-Za-z0-9_]+)((?:\s+\[[^\]]+\])*)$/gm)
	{
		$all_symbols{$1} = 1;
		$global_symbols{$1} = 1;
	}
	my %signatures;
	while ($data =~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/gm)
	{
		$all_symbols{$1} = 1;
		$function_symbols{$1} = 1;
		my ($ok, $params_or_error) = parse_lowir_param_list($2);
		if (!$ok)
		{
			push @errors, "function \@$1 has invalid parameter list: $params_or_error";
			next;
		}
		my ($metadata_ok, $metadata_or_error) =
			parse_lowir_function_metadata_suffix($4);
		if (!$metadata_ok)
		{
			push @errors, "function \@$1 has invalid metadata: $metadata_or_error";
			next;
		}
		$signatures{$1} = {
			ret => $3,
			params => $params_or_error,
			arity => $metadata_or_error->{arity},
		};
	}
	while ($data =~ /^declare function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*)$/gm)
	{
		$all_symbols{$1} = 1;
		$function_symbols{$1} = 1;
		my ($ok, $params_or_error) = parse_lowir_param_list($2);
		if (!$ok)
		{
			push @errors, "declare function \@$1 has invalid parameter list: $params_or_error";
			next;
		}
		my ($metadata_ok, $metadata_or_error) =
			parse_lowir_function_metadata_suffix($4);
		if (!$metadata_ok)
		{
			push @errors, "declare function \@$1 has invalid metadata: $metadata_or_error";
			next;
		}
		$signatures{$1} = {
			ret => $3,
			params => $params_or_error,
			arity => $metadata_or_error->{arity},
		};
	}

	my %missing_alias_targets;
	for my $name (@alias_targets)
	{
		$missing_alias_targets{$name} = 1 if !exists($all_symbols{$name});
	}
	push @errors, "object alias target(s) are not top-level symbols: " .
		join(', ', map { "\@$_" } sort keys(%missing_alias_targets))
		if scalar(keys(%missing_alias_targets)) > 0;

	my %missing_calls;
	while ($data =~ /call\s+(?:void|$type_pattern)\s+@([A-Za-z0-9_]+)\(/g)
	{
		my $name = $1;
		next if exists($function_symbols{$name});
		next if lowir_symbol_is_allowed_external($name, $source_text, 0);
		$missing_calls{$name} = 1;
	}
	push @errors, "undefined call target(s): " . join(', ', sort keys(%missing_calls))
		if scalar(keys(%missing_calls)) > 0;

	my %missing_addrs;
	while ($data =~ /addr\s+@([A-Za-z0-9_]+)/g)
	{
		my $name = $1;
		next if exists($all_symbols{$name});
		next if lowir_symbol_is_allowed_external($name, $source_text, 1);
		$missing_addrs{$name} = 1;
	}
	push @errors, "undefined address-taken symbol(s): " . join(', ', sort keys(%missing_addrs))
		if scalar(keys(%missing_addrs)) > 0;

	push @errors, validate_lowir_structure($data,
		$source_text,
		\%all_symbols,
		\%function_symbols,
		\%global_symbols,
		\%global_types,
		\%signatures);
	push @errors, validate_lowir_roles($data);

	return scalar(@errors) == 0 ? (1, undef) : (0, join('; ', @errors));
}

sub lowir_metadata_item_ignored_for_compare
{
	my ($key, $value) = @_;
	# Validate full metadata, but do not make early source-to-LowIR oracles depend
	# on later object/export policy or optional optimizer/provenance annotations.
	return 1 if $key =~ /^(?:linkage|binding|object|tls_for|keep_alias|prefer_local|trivial_lifecycle)$/;
	return 1 if $key =~ /^(?:effects|unwind|return|capture|access|alias|projection)$/;
	return 1 if $key eq 'storage' && $value =~ /^(?:readonly|writable)$/;
	return 0;
}

sub canonicalize_lowir_metadata_group_for_compare
{
	my ($group) = @_;
	my @kept;
	for my $item (split(/\s*,\s*/, $group))
	{
		my ($key, $value) = ($item =~ /^([A-Za-z0-9_]+)\s*=\s*([^\],\s]+)$/);
		next if defined($key) && lowir_metadata_item_ignored_for_compare($key, $value);
		push @kept, $item;
	}
	return scalar(@kept) == 0 ? '' : ' [' . join(', ', @kept) . ']';
}

sub canonicalize_lowir_for_compare
{
	my ($data, $function_symbols) = @_;
	return undef if !defined($data);
	$data =~ s/\s+\[([^\]]+)\]/canonicalize_lowir_metadata_group_for_compare($1)/ge;
	# Object aliases are backend symbol-surface metadata, like object=... metadata.
	# Validate them above, but omit them from relaxed source-to-LowIR comparison.
	$data =~ s/^alias object [^\n]*\n?//gm;
	if (!defined($function_symbols))
	{
		my %local_function_symbols;
		my $next_function_symbol = 0;
		while ($data =~ /^(?:declare\s+)?function @([A-Za-z0-9_]+)\b/gm)
		{
			my $name = $1;
			next if exists($local_function_symbols{$name});
			$local_function_symbols{$name} = '<fn' . $next_function_symbol . '>';
			++$next_function_symbol;
		}
		$function_symbols = \%local_function_symbols;
	}
	$data =~ s/@([A-Za-z0-9_]+)\b/exists($function_symbols->{$1}) ? '@' . $function_symbols->{$1} : '@' . $1/ge;
	$data =~ s/[ \t]+$//mg;
	return $data;
}

sub lowir_function_symbol_records
{
	my ($data) = @_;
	my %records;
	my @ordered;
	my $type_pattern = lowir_type_pattern();

	for my $line (split(/\n/, $data))
	{
		my ($name, $params, $return_type, $metadata_suffix);
		if ($line =~ /^declare function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*)$/)
		{
			($name, $params, $return_type, $metadata_suffix) = ($1, $2, $3, $4);
		}
		elsif ($line =~ /^function @([A-Za-z0-9_]+)\((.*?)\) -> ($type_pattern)((?:\s+\[[^\]]+\])*) \{$/)
		{
			($name, $params, $return_type, $metadata_suffix) = ($1, $2, $3, $4);
		}
		else
		{
			next;
		}

		if (!exists($records{$name}))
		{
			$records{$name} = {
				name => $name,
				order => scalar(@ordered),
				signature => lowir_normalize_function_signature($params, $return_type),
				keys => {},
			};
			push @ordered, $name;
		}

		my ($ok, $metadata_or_error) =
			parse_lowir_function_metadata_suffix($metadata_suffix);
		next if !$ok;
		$records{$name}{keys}{"role:$metadata_or_error->{role}"} = 1
			if exists($metadata_or_error->{role}) && $metadata_or_error->{role} ne '';
		$records{$name}{keys}{"object:$metadata_or_error->{object}"} = 1
			if exists($metadata_or_error->{object}) && $metadata_or_error->{object} ne '';
	}

	return (\%records, \@ordered);
}

sub lowir_normalize_function_signature
{
	my ($params, $return_type) = @_;
	$params = lowir_trim($params);
	$return_type = lowir_trim($return_type);
	$params =~ s/\s+/ /g;
	$return_type =~ s/\s+/ /g;
	return '(' . $params . ') -> ' . $return_type;
}

sub assign_lowir_function_symbol_pair
{
	my ($ref_map, $my_map, $ref_name, $my_name, $next_ref) = @_;
	return $$next_ref if exists($ref_map->{$ref_name}) || exists($my_map->{$my_name});
	my $placeholder = '<fn' . $$next_ref . '>';
	++$$next_ref;
	$ref_map->{$ref_name} = $placeholder;
	$my_map->{$my_name} = $placeholder;
	return $$next_ref;
}

sub lowir_function_entry_map
{
	my ($data) = @_;
	my %map;
	for my $entry (split_lowir_top_level_entries($data))
	{
		next if $entry !~ /^function \@([A-Za-z0-9_]+)\(/;
		$map{$1} = $entry;
	}
	return \%map;
}

sub lowir_function_shape_text
{
	my ($entry) = @_;
	my @lines = split(/\n/, $entry, -1);
	# Drop the function-level [..] metadata (object=, binding=, role=, ...) from
	# the header so the shape ignores volatile mangle/linkage details. Parameter
	# brackets (e.g. [pass=reference]) sit inside the parens and are preserved.
	$lines[0] =~ s/((?:\s+\[[^\]]+\])+)\s*\{\s*$/ {/ if @lines;
	my $text = join("\n", @lines);
	# Mask every symbol reference (the function's own name, callees, globals) so
	# two structurally identical functions match regardless of their mangled names.
	$text =~ s/\@[^\s(),]+/\@<sym>/g;
	return $text;
}

sub paired_lowir_function_symbol_maps
{
	my ($ref_data, $my_data) = @_;
	my ($ref_records, $ref_order) = lowir_function_symbol_records($ref_data);
	my ($my_records, $my_order) = lowir_function_symbol_records($my_data);
	my (%ref_map, %my_map);
	my $next = 0;

	my %ref_by_key;
	for my $name (@$ref_order)
	{
		for my $key (keys(%{$ref_records->{$name}{keys}}))
		{
			push @{$ref_by_key{$key}}, $name;
		}
	}
	my %my_by_key;
	for my $name (@$my_order)
	{
		for my $key (keys(%{$my_records->{$name}{keys}}))
		{
			push @{$my_by_key{$key}}, $name;
		}
	}
	for my $key (sort keys(%ref_by_key))
	{
		next if !exists($my_by_key{$key});
		next if scalar(@{$ref_by_key{$key}}) != 1 || scalar(@{$my_by_key{$key}}) != 1;
		$next = assign_lowir_function_symbol_pair(\%ref_map,
		                                          \%my_map,
		                                          $ref_by_key{$key}[0],
		                                          $my_by_key{$key}[0],
		                                          \$next);
	}

	for my $name (sort keys(%$ref_records))
	{
		next if !exists($my_records->{$name});
		next if $ref_records->{$name}{signature} ne $my_records->{$name}{signature};
		$next = assign_lowir_function_symbol_pair(\%ref_map,
		                                          \%my_map,
		                                          $name,
		                                          $name,
		                                          \$next);
	}

	# Structural pass: pair any still-unpaired *defined* functions whose
	# normalized shape (signature + body with symbol names masked) is unique on
	# both sides. This lets the relaxed compare line functions up by structure
	# rather than falling back to emission order, so a function whose mangled
	# name differs (or is absent) no longer forces an order-sensitive mispairing.
	# Requiring the shape to be unique on both sides keeps this sound: the
	# correspondence is forced, and a genuinely different body has a different
	# shape and stays unpaired, so a real codegen diff is never hidden.
	my $ref_entries = lowir_function_entry_map($ref_data);
	my $my_entries = lowir_function_entry_map($my_data);
	my (%ref_by_shape, %my_by_shape);
	for my $name (@$ref_order)
	{
		next if exists($ref_map{$name}) || !exists($ref_entries->{$name});
		push @{$ref_by_shape{lowir_function_shape_text($ref_entries->{$name})}}, $name;
	}
	for my $name (@$my_order)
	{
		next if exists($my_map{$name}) || !exists($my_entries->{$name});
		push @{$my_by_shape{lowir_function_shape_text($my_entries->{$name})}}, $name;
	}
	for my $shape (sort keys(%ref_by_shape))
	{
		next if !exists($my_by_shape{$shape});
		next if scalar(@{$ref_by_shape{$shape}}) != 1 || scalar(@{$my_by_shape{$shape}}) != 1;
		$next = assign_lowir_function_symbol_pair(\%ref_map,
		                                          \%my_map,
		                                          $ref_by_shape{$shape}[0],
		                                          $my_by_shape{$shape}[0],
		                                          \$next);
	}

	my @remaining_ref = grep { !exists($ref_map{$_}) } @$ref_order;
	my @remaining_my = grep { !exists($my_map{$_}) } @$my_order;
	my $pairs = scalar(@remaining_ref) < scalar(@remaining_my) ?
		scalar(@remaining_ref) : scalar(@remaining_my);
	for (my $i = 0; $i < $pairs; ++$i)
	{
		$next = assign_lowir_function_symbol_pair(\%ref_map,
		                                          \%my_map,
		                                          $remaining_ref[$i],
		                                          $remaining_my[$i],
		                                          \$next);
	}
	for my $name (@remaining_ref)
	{
		next if exists($ref_map{$name});
		$ref_map{$name} = '<fn' . $next . '>';
		++$next;
	}
	for my $name (@remaining_my)
	{
		next if exists($my_map{$name});
		$my_map{$name} = '<fn' . $next . '>';
		++$next;
	}

	return (\%ref_map, \%my_map);
}

sub lowir_relaxed_top_level_rank
{
	my ($entry) = @_;
	return 0 if $entry =~ /^declare global /;
	return 1 if $entry =~ /^declare function /;
	return 2 if $entry =~ /^global /;
	return 3 if $entry =~ /^function /;
	return 4 if $entry =~ /^alias object /;
	return 5;
}

sub split_lowir_top_level_entries
{
	my ($data) = @_;
	my @lines = split(/\n/, $data);
	my @entries;
	my @current;
	my $mode = '';
	for (my $i = 0; $i < scalar(@lines); ++$i)
	{
		my $line = $lines[$i];
		next if $line =~ /^\s*$/ && scalar(@current) == 0;
		if ($mode eq '')
		{
			if ($line =~ /^(?:declare global|declare function) /)
			{
				push @entries, $line;
				next;
			}
			if ($line =~ /^alias object /)
			{
				push @entries, $line;
				next;
			}
			if ($line =~ /^global .*=\s*\{$/)
			{
				@current = ($line);
				$mode = 'global';
				next;
			}
			if ($line =~ /^global /)
			{
				push @entries, $line;
				next;
			}
			if ($line =~ /^function /)
			{
				@current = ($line);
				$mode = 'function';
				next;
			}
			push @entries, $line;
			next;
		}

		push @current, $line;
		if ($line eq '}')
		{
			push @entries, join("\n", @current);
			@current = ();
			$mode = '';
		}
	}
	push @entries, join("\n", @current) if scalar(@current) != 0;
	return @entries;
}

sub canonicalize_lowir_top_level_order_for_compare
{
	my ($data) = @_;
	my @entries = split_lowir_top_level_entries($data);
	@entries = sort {
		my $rank_a = lowir_relaxed_top_level_rank($a);
		my $rank_b = lowir_relaxed_top_level_rank($b);
		$rank_a <=> $rank_b || $a cmp $b
	} @entries;
	return join("\n\n", @entries) . (scalar(@entries) ? "\n" : "");
}

sub canonicalize_lowir_pair_for_compare
{
	my ($ref_data, $my_data) = @_;
	my ($ref_symbols, $my_symbols) =
		paired_lowir_function_symbol_maps($ref_data, $my_data);
	my $ref_compare_data =
		canonicalize_lowir_top_level_order_for_compare(
			canonicalize_lowir_for_compare($ref_data, $ref_symbols));
	my $my_compare_data =
		canonicalize_lowir_top_level_order_for_compare(
			canonicalize_lowir_for_compare($my_data, $my_symbols));
	return ($ref_compare_data, $my_compare_data);
}

sub lowir_compare_artifact_paths
{
	my ($my) = @_;
	return (
		"$my.lowir.ref.compare",
		"$my.lowir.my.compare",
		"$my.lowir.compare.diff",
	);
}

sub clear_lowir_compare_failure_artifacts
{
	my ($my) = @_;
	my ($ref_compare, $my_compare, $diff_path) =
		lowir_compare_artifact_paths($my);
	unlink($ref_compare);
	unlink($my_compare);
	unlink($diff_path);
}

sub write_lowir_compare_failure_artifacts
{
	my ($my, $ref_compare_data, $my_compare_data) = @_;
	my ($ref_compare, $my_compare, $diff_path) =
		lowir_compare_artifact_paths($my);
	putrawdata($ref_compare, $ref_compare_data);
	putrawdata($my_compare, $my_compare_data);

	my $diff_data = '';
	if (open(my $diff_fh, '-|', 'diff', '-u', $ref_compare, $my_compare))
	{
		local $/;
		$diff_data = <$diff_fh>;
		close($diff_fh);
	}
	putrawdata($diff_path, defined($diff_data) ? $diff_data : '');
	return ($ref_compare, $my_compare, $diff_path);
}

sub lowir_compare_failure_hint
{
	my ($ref_compare, $my_compare, $diff_path) = @_;
	return "To inspect the relaxed LowIR comparison:\n\n    \$ diff " .
		rooted_path($ref_compare) . " " . rooted_path($my_compare) .
		"\n    \$ cat " . rooted_path($diff_path) . "\n\n" .
		"These files contain the canonicalized LowIR used for relaxed comparison.\n\n";
}

sub compare_lowir_text
{
	my ($ref_suffix, $my_suffix, $testbase) = @_;
	my $ref = "$testbase.$ref_suffix";
	my $my = "$testbase.$my_suffix";
	my $source_file = "$testbase.t";
	clear_lowir_compare_failure_artifacts($my);
	my $ref_status = getdata("$ref.exit_status");
	my $my_status = getdata("$my.exit_status");
	my @missing_status = ();
	push @missing_status, "$ref.exit_status" if !defined($ref_status);
	push @missing_status, "$my.exit_status" if !defined($my_status);
	return (0, "ERROR: missing exit status output (" . join(', ', @missing_status) . ")")
		if scalar(@missing_status) != 0;
	return (0, status_mismatch_message("tool", $ref_status, $my_status))
		if $ref_status ne $my_status;
	return (1, undef) if $ref_status ne 'EXIT_SUCCESS';

	my $ref_data = getdata($ref);
	my $my_data = getdata($my);
	my @missing_output = ();
	push @missing_output, $ref if !defined($ref_data);
	push @missing_output, $my if !defined($my_data);
	return (0, "ERROR: missing output file (" . join(', ', @missing_output) . ")")
		if scalar(@missing_output) != 0;

	my $direct_compare = env_flag_enabled('CPPGM_LOWIR_DIRECT_TEXT_COMPARE');
	my ($ref_valid, $ref_error) = validate_lowir_text(
		$ref_data,
		$source_file,
		{ strict_presentation_order => 1 });
	return (0, "ERROR: invalid reference LowIR: $ref_error") if !$ref_valid;
	my ($my_valid, $my_error) = validate_lowir_text(
		$my_data,
		$source_file,
		{ strict_presentation_order => $direct_compare });
	return (0, "ERROR: generated LowIR failed sanity validation: $my_error") if !$my_valid;

	if ($direct_compare)
	{
		return ($ref_data eq $my_data
			? (1, undef)
			: (0, "ERROR: generated LowIR does not match reference with direct text compare"));
	}

	my ($ref_compare_data, $my_compare_data) =
		canonicalize_lowir_pair_for_compare($ref_data, $my_data);

	return (1, undef, undef) if $ref_compare_data eq $my_compare_data;

	my ($ref_compare, $my_compare, $diff_path) =
		write_lowir_compare_failure_artifacts($my,
		                                      $ref_compare_data,
		                                      $my_compare_data);
	return (0,
	        "ERROR: generated LowIR does not match reference after relaxed metadata canonicalization and order canonicalization; canonical diff written to $diff_path",
	        lowir_compare_failure_hint($ref_compare, $my_compare, $diff_path));
}

sub compare_witness_text
{
	my ($ref_suffix, $my_suffix, $testbase) = @_;
	my $ref = "$testbase.$ref_suffix";
	my $my = "$testbase.$my_suffix";
	my $ref_witness = "$ref.witness";
	my $my_witness = "$my.witness";

	return ('skip', undef) if !-e $ref_witness;

	my $ref_status = getdata("$ref.exit_status");
	return ('skip', undef)
		if defined($ref_status) && $ref_status ne 'EXIT_SUCCESS';

	my $my_status = getdata("$my.exit_status");
	return ('fail', "ERROR: missing generated exit status ($my.exit_status)")
		if !defined($my_status);
	return ('fail', "ERROR: witness exited $my_status")
		if $my_status ne 'EXIT_SUCCESS';

	my $ref_data = getrawdata($ref_witness);
	my $my_data = getrawdata($my_witness);
	my @missing_output = ();
	push @missing_output, $ref_witness if !defined($ref_data);
	push @missing_output, $my_witness if !defined($my_data);
	return ('fail', "ERROR: missing witness output file (" . join(', ', @missing_output) . ")")
		if scalar(@missing_output) != 0;

	return ('ok', undef) if $ref_data eq $my_data;

	my $diff_path = "$my_witness.diff";
	my $diff_data = '';
	if (open(my $diff_fh, '-|', 'diff', '-u', $ref_witness, $my_witness))
	{
		local $/;
		$diff_data = <$diff_fh>;
		close($diff_fh);
	}
	putrawdata($diff_path, defined($diff_data) ? $diff_data : '');
	return ('fail', "ERROR: witness output does not match reference");
}

sub canonical_exit_status
{
	my ($status) = @_;
	return undef if !defined($status);
	return 'EXIT_SUCCESS' if $status eq '0' || $status eq 'EXIT_SUCCESS';
	return 'EXIT_TIMEOUT' if $status eq '124' || $status eq 'EXIT_TIMEOUT';
	return 'EXIT_OOM' if $status eq '125' || $status eq 'EXIT_OOM';
	return 'EXIT_NOT_IMPLEMENTED' if $status eq '86' || $status eq 'EXIT_NOT_IMPLEMENTED';
	return 'EXIT_FAILURE' if $status eq '1' || $status eq 'EXIT_FAILURE';
	return $status;
}

sub format_exit_status
{
	my ($status) = @_;
	return '<missing>' if !defined($status);
	my $canonical = canonical_exit_status($status);
	return $status if !defined($canonical) || $canonical eq $status;
	return "$canonical ($status)";
}

sub status_mismatch_message
{
	my ($label, $expected_raw, $got_raw) = @_;
	my $expected = canonical_exit_status($expected_raw);
	my $got = canonical_exit_status($got_raw);
	my $expected_text = format_exit_status($expected_raw);
	my $got_text = format_exit_status($got_raw);
	if (defined($got) && $got eq 'EXIT_TIMEOUT' &&
	    (!defined($expected) || $expected ne 'EXIT_TIMEOUT'))
	{
		return "ERROR: $label timed out (expected $expected_text, got $got_text)";
	}
	if (defined($expected) && $expected eq 'EXIT_TIMEOUT' &&
	    (!defined($got) || $got ne 'EXIT_TIMEOUT'))
	{
		return "ERROR: $label did not time out as expected (expected $expected_text, got $got_text)";
	}
	if (defined($got) && $got eq 'EXIT_OOM' &&
	    (!defined($expected) || $expected ne 'EXIT_OOM'))
	{
		return "ERROR: $label ran out of memory (expected $expected_text, got $got_text)";
	}
	if (defined($expected) && $expected eq 'EXIT_OOM' &&
	    (!defined($got) || $got ne 'EXIT_OOM'))
	{
		return "ERROR: $label did not run out of memory as expected (expected $expected_text, got $got_text)";
	}
	return "ERROR: $label exit status mismatch (expected $expected_text, got $got_text)";
}

sub compare_program_outputs
{
	my ($ref_prefix, $my_prefix, $label, $allow_missing_stdout) = @_;
	$label = 'generated' if !defined($label);
	my $ref_impl_raw = getdata("$ref_prefix.impl.exit_status");
	my $my_impl_raw = getdata("$my_prefix.impl.exit_status");
	my $ref_impl = canonical_exit_status($ref_impl_raw);
	my $my_impl = canonical_exit_status($my_impl_raw);
	return (0, status_mismatch_message("implementation", $ref_impl_raw, $my_impl_raw))
		if !defined($ref_impl) || !defined($my_impl) || $ref_impl ne $my_impl;
	return (1, undef) if $ref_impl ne 'EXIT_SUCCESS';
	my $ref_program_status = getdata("$ref_prefix.program.exit_status");
	my $my_program_status = getdata("$my_prefix.program.exit_status");
	my $ref_program_stdout = getdata("$ref_prefix.program.stdout");
	my $my_program_stdout = getdata("$my_prefix.program.stdout");
	if ($allow_missing_stdout)
	{
		$ref_program_stdout = '' if !defined($ref_program_stdout);
		$my_program_stdout = '' if !defined($my_program_stdout);
	}
	my @missing_program = ();
	push @missing_program, "$ref_prefix.program.exit_status" if !defined($ref_program_status);
	push @missing_program, "$my_prefix.program.exit_status" if !defined($my_program_status);
	return (0, "ERROR: missing program output (" . join(', ', @missing_program) . ")")
		if scalar(@missing_program) != 0;
	my $ref_program = canonical_exit_status($ref_program_status);
	my $my_program = canonical_exit_status($my_program_status);
	return (0, status_mismatch_message("$label program", $ref_program_status, $my_program_status))
		if !defined($ref_program) || !defined($my_program) || $ref_program ne $my_program;
	@missing_program = ();
	push @missing_program, "$ref_prefix.program.stdout" if !defined($ref_program_stdout);
	push @missing_program, "$my_prefix.program.stdout" if !defined($my_program_stdout);
	return (0, "ERROR: missing program output (" . join(', ', @missing_program) . ")")
		if scalar(@missing_program) != 0;
	return (1, undef)
		if ($ref_program_stdout eq $my_program_stdout);
	return (0, "ERROR: $label program stdout does not match reference");
}

sub host_platform_tag
{
	chomp(my $os = `uname -s 2>/dev/null`);
	$os = lc($os);
	return 'macos' if $os eq 'darwin';
	return 'linux' if $os eq 'linux';
	return $os;
}

sub compare_optional_inspect
{
	my ($ref_prefix, $my_prefix, $host_tag) = @_;
	my @ref_candidates = ();
	push @ref_candidates, "$ref_prefix.inspect.$host_tag" if defined($host_tag) && $host_tag ne '';
	push @ref_candidates, "$ref_prefix.inspect";
	my $ref_file;
	for my $candidate (@ref_candidates)
	{
		if (-f $candidate)
		{
			$ref_file = $candidate;
			last;
		}
	}
	return (1, undef, undef) if !defined($ref_file);

	my $my_file = "$my_prefix.inspect";
	my $ref_data = getdata($ref_file);
	my $my_data = getdata($my_file);
	return (0, "ERROR: missing inspect output", "To compare inspect output:\n\n    \$ diff " .
		rooted_path($ref_file) . " " . rooted_path($my_file) . "\n\n")
		if !defined($ref_data) || !defined($my_data);
	return ($ref_data eq $my_data ? (1, undef, undef)
		: (0, "ERROR: inspect output does not match reference",
		   "To compare inspect output:\n\n    \$ diff " .
		   rooted_path($ref_file) . " " . rooted_path($my_file) . "\n\n"));
}

if (scalar(@ARGV) == 2 && $ARGV[0] eq 'canonicalize-machine-ir')
{
	my $path = $ARGV[1];
	my $data;
	if ($path eq '-')
	{
		local $/;
		$data = <STDIN>;
		$data = '' if !defined($data);
	}
	else
	{
		$data = getrawdata($path);
		die "Unable to read $path\n" if !defined($data);
	}
	print canonicalize_machine_ir($data);
	exit 0;
}

if (scalar(@ARGV) == 2 && $ARGV[0] eq 'normalize-machine-ir')
{
	my $path = $ARGV[1];
	my $data;
	if ($path eq '-')
	{
		local $/;
		$data = <STDIN>;
		$data = '' if !defined($data);
	}
	else
	{
		$data = getrawdata($path);
		die "Unable to read $path\n" if !defined($data);
	}
	print normalize_machine_ir($data);
	exit 0;
}

if (scalar(@ARGV) != 4)
{
	die "Usage: compare_results_common.pl <mode> <ref_suffix> <my_suffix> <testlocation>";
}

my $mode = shift(@ARGV);
my $ref_suffix = $ARGV[0];
my $my_suffix = $ARGV[1];
my $tests = $ARGV[2];
my $verbose = $ENV{VERBOSE} || $ENV{CPGM_TEST_VERBOSE};
my $requested_keep_going = $ENV{KEEP_GOING};
my $auto_check_keep_going = env_flag_enabled('CPPGM_CHECK_AUTO_KEEP_GOING');
my $check_mode = env_flag_enabled('CPPGM_CHECK_MODE');
my $cwd = getcwd();
my $assignment = basename($cwd);
my $repo_root = dirname($cwd);
my $host_tag = host_platform_tag();

my %patterns = (
	text_t => qr/\.t$/,
	lowir_t => qr/\.t$/,
	witness_t => qr/\.t$/,
	text_t1 => qr/\.t\.1$/,
	program_t1 => qr/\.t\.1$/,
	mir_t => qr/\.t$/,
	mir_canonical_t => qr/\.t$/,
	mir_structural_t => qr/\.t$/,
	map_struct_t => qr/\.t$/,
	map_exact_t => qr/\.t$/,
	link_program_t => qr/\.t$/,
);
die "Unsupported compare_results mode $mode" if !exists($patterns{$mode});

my @tests = collect_tests($tests, $patterns{$mode});
my $suite_total = scalar(@tests);
my $keep_going = $requested_keep_going || ($auto_check_keep_going && $suite_total > 1);
my $npass = 0;
my $failed = 0;
my $witness_compared = 0;
my $witness_failures = 0;
my $witness_skipped = 0;

sub compare_label
{
	return "$assignment check" if $check_mode;
	return "$assignment $tests";
}

sub rooted_path
{
	my ($path) = @_;
	return $path =~ m{^/} ? $path : "$cwd/$path";
}

sub fail_prefix
{
	return compare_label() . ": FAIL after $npass/$suite_total passed\n";
}

sub rerun_hint
{
	return "To rerun this assignment with per-test output from the repo root:\n\n    \$ cd $repo_root && VERBOSE=1 make $assignment\n\n";
}

sub checked_output_hint
{
	my ($ref_path, $my_path) = @_;
	return "To inspect this checked output:\n\n    \$ cat " .
		rooted_path("$ref_path.exit_status") . " " .
		rooted_path("$my_path.exit_status") . "\n    \$ diff " .
		rooted_path($ref_path) . " " . rooted_path($my_path) . "\n\n";
}

sub witness_output_hint
{
	my ($ref_path, $my_path) = @_;
	return "To inspect this witness output:\n\n    \$ diff " .
		rooted_path($ref_path) . " " . rooted_path($my_path) .
		"\n    \$ cat " . rooted_path("$my_path.diff") . "\n\n";
}

sub program_output_hint
{
	my ($ref_prefix, $my_prefix) = @_;
	return "To inspect this program result:\n\n    \$ cat " .
		rooted_path("$ref_prefix.impl.exit_status") . " " .
		rooted_path("$my_prefix.impl.exit_status") . "\n    \$ cat " .
		rooted_path("$ref_prefix.program.exit_status") . " " .
		rooted_path("$my_prefix.program.exit_status") . "\n    \$ diff " .
		rooted_path("$ref_prefix.program.stdout") . " " .
		rooted_path("$my_prefix.program.stdout") . "\n\n";
}

for my $test (@tests)
{
	print "\n$test: " if $verbose;
	my $display_test = $keep_going ? "$assignment/$test" : $test;
	my $testbase = $test;
	$testbase =~ s/\.t$//;
	$testbase =~ s/\.t\.1$//;

	if ($mode eq 'witness_t')
	{
		my ($state, $message) = compare_witness_text($ref_suffix, $my_suffix, $testbase);
		if ($state eq 'skip')
		{
			++$witness_skipped;
			print "SKIP\n\n" if $verbose;
			next;
		}

		++$witness_compared;
		if ($state eq 'ok')
		{
			++$npass;
			print "PASS\n\n" if $verbose;
			next;
		}

		++$witness_failures;
		if ($keep_going)
		{
			print "$display_test: $message\n";
			print witness_output_hint("$testbase.$ref_suffix.witness",
			                          "$testbase.$my_suffix.witness")
				if $check_mode && $auto_check_keep_going;
			$failed = 1;
			next;
		}

		print fail_prefix();
		print "$test: $message\n\n";
		print rerun_hint();
		print witness_output_hint("$testbase.$ref_suffix.witness",
		                          "$testbase.$my_suffix.witness");
		print "TEST FAIL\n";
		exit(1);
	}

	my ($ok, $message, $hint);
	eval {
		if ($mode eq 'text_t')
		{
			($ok, $message) = compare_text(qr/\.t$/, $ref_suffix, $my_suffix, $testbase, 0);
			$hint = checked_output_hint("$testbase.$ref_suffix", "$testbase.$my_suffix");
		}
		elsif ($mode eq 'lowir_t')
		{
			($ok, $message, $hint) =
				compare_lowir_text($ref_suffix, $my_suffix, $testbase);
			$hint = checked_output_hint("$testbase.$ref_suffix", "$testbase.$my_suffix")
				if !defined($hint);
		}
		elsif ($mode eq 'text_t1')
		{
			($ok, $message) = compare_text(qr/\.t\.1$/, $ref_suffix, $my_suffix, $testbase, 1);
			$hint = checked_output_hint("$testbase.$ref_suffix", "$testbase.$my_suffix");
		}
		elsif ($mode eq 'program_t1')
		{
			($ok, $message) = compare_program_outputs("$testbase.$ref_suffix", "$testbase.$my_suffix");
			$hint = program_output_hint("$testbase.$ref_suffix", "$testbase.$my_suffix");
		}
		elsif ($mode eq 'mir_t')
		{
			my $ref = "$testbase.$ref_suffix";
			my $my = "$testbase.$my_suffix";
			my $ref_impl_raw = getdata("$ref.impl.exit_status");
			my $my_impl_raw = getdata("$my.impl.exit_status");
			my $ref_impl = canonical_exit_status($ref_impl_raw);
			my $my_impl = canonical_exit_status($my_impl_raw);
			($ok, $message) = (0, status_mismatch_message("implementation", $ref_impl_raw, $my_impl_raw))
				if !defined($ref_impl) || !defined($my_impl) || $ref_impl ne $my_impl;
			if (!defined($ok))
			{
				if ($ref_impl ne 'EXIT_SUCCESS')
				{
					($ok, $message) = (1, undef);
				}
				elsif ((-f "$ref.mir") &&
				       (normalize_machine_ir(getdata("$ref.mir")) ne normalize_machine_ir(getdata("$my.mir"))))
				{
					($ok, $message) = (0, "ERROR: machine IR dumps do not match (.mir)");
					$hint = "To compare machine IR dumps:\n\n    \$ diff " .
						rooted_path("$ref.mir") . " " . rooted_path("$my.mir") . "\n\n";
				}
				else
				{
					my ($prog_ok, $prog_message) = compare_program_outputs($ref, $my, 'generated');
					($ok, $message) = ($prog_ok, $prog_message);
					$hint = program_output_hint($ref, $my);
				}
			}
		}
		elsif ($mode eq 'mir_canonical_t')
		{
			my $ref = "$testbase.$ref_suffix";
			my $my = "$testbase.$my_suffix";
			my $ref_impl_raw = getdata("$ref.impl.exit_status");
			my $my_impl_raw = getdata("$my.impl.exit_status");
			my $ref_impl = canonical_exit_status($ref_impl_raw);
			my $my_impl = canonical_exit_status($my_impl_raw);
			($ok, $message) = (0, status_mismatch_message("implementation", $ref_impl_raw, $my_impl_raw))
				if !defined($ref_impl) || !defined($my_impl) || $ref_impl ne $my_impl;
			if (!defined($ok))
			{
				if ($ref_impl ne 'EXIT_SUCCESS')
				{
					($ok, $message) = (1, undef);
				}
				else
				{
					my ($mir_ok, $mir_message, $mir_hint) = compare_canonical_machine_ir($ref, $my);
					if (!$mir_ok)
					{
						($ok, $message, $hint) = ($mir_ok, $mir_message, $mir_hint);
					}
					else
					{
						my ($prog_ok, $prog_message) = compare_program_outputs($ref, $my, 'generated');
						($ok, $message) = ($prog_ok, $prog_message);
						$hint = program_output_hint($ref, $my);
					}
				}
			}
		}
		elsif ($mode eq 'mir_structural_t')
		{
			my $ref = "$testbase.$ref_suffix";
			my $my = "$testbase.$my_suffix";
			my $ref_impl_raw = getdata("$ref.impl.exit_status");
			my $my_impl_raw = getdata("$my.impl.exit_status");
			my $ref_impl = canonical_exit_status($ref_impl_raw);
			my $my_impl = canonical_exit_status($my_impl_raw);
			($ok, $message) = (0, status_mismatch_message("implementation", $ref_impl_raw, $my_impl_raw))
				if !defined($ref_impl) || !defined($my_impl) || $ref_impl ne $my_impl;
			if (!defined($ok))
			{
				if ($ref_impl ne 'EXIT_SUCCESS')
				{
					($ok, $message) = (1, undef);
				}
				else
				{
					my ($mir_ok, $mir_message, $mir_hint) = compare_structural_machine_ir($ref, $my);
					if (!$mir_ok)
					{
						($ok, $message, $hint) = ($mir_ok, $mir_message, $mir_hint);
					}
					else
					{
						my ($prog_ok, $prog_message) = compare_program_outputs($ref, $my, 'generated');
						($ok, $message) = ($prog_ok, $prog_message);
						$hint = program_output_hint($ref, $my);
					}
				}
			}
		}
		elsif ($mode eq 'map_struct_t')
		{
			my $ref = "$testbase.$ref_suffix";
			my $my = "$testbase.$my_suffix";
			my $ref_impl_raw = getdata("$ref.impl.exit_status");
			my $my_impl_raw = getdata("$my.impl.exit_status");
			my $ref_impl = canonical_exit_status($ref_impl_raw);
			my $my_impl = canonical_exit_status($my_impl_raw);
			($ok, $message) = (0, status_mismatch_message("implementation", $ref_impl_raw, $my_impl_raw))
				if !defined($ref_impl) || !defined($my_impl) || $ref_impl ne $my_impl;
			if (!defined($ok))
			{
				if ($ref_impl ne 'EXIT_SUCCESS')
				{
					($ok, $message) = (1, undef);
				}
				elsif ((-f "$ref.map") && !compare_link_maps_structural("$ref.map", "$my.map"))
				{
					($ok, $message) = (0, "ERROR: link maps do not match structurally");
					$hint = "To compare link maps:\n\n    \$ diff " .
						rooted_path("$ref.map") . " " . rooted_path("$my.map") . "\n\n";
				}
				else
				{
					my ($prog_ok, $prog_message) = compare_program_outputs($ref, $my, 'linked');
					($ok, $message) = ($prog_ok, $prog_message);
					$hint = program_output_hint($ref, $my);
				}
			}
		}
		elsif ($mode eq 'map_exact_t')
		{
			my $ref = "$testbase.$ref_suffix";
			my $my = "$testbase.$my_suffix";
			my $ref_impl_raw = getdata("$ref.impl.exit_status");
			my $my_impl_raw = getdata("$my.impl.exit_status");
			my $ref_impl = canonical_exit_status($ref_impl_raw);
			my $my_impl = canonical_exit_status($my_impl_raw);
			($ok, $message) = (0, status_mismatch_message("implementation", $ref_impl_raw, $my_impl_raw))
				if !defined($ref_impl) || !defined($my_impl) || $ref_impl ne $my_impl;
			if (!defined($ok))
			{
				if ($ref_impl ne 'EXIT_SUCCESS')
				{
					($ok, $message) = (1, undef);
				}
				elsif ((-f "$ref.map") && (getdata("$ref.map") ne getdata("$my.map")))
				{
					($ok, $message) = (0, "ERROR: link maps do not match");
					$hint = "To compare link maps:\n\n    \$ diff " .
						rooted_path("$ref.map") . " " . rooted_path("$my.map") . "\n\n";
				}
				else
				{
					my ($prog_ok, $prog_message) = compare_program_outputs($ref, $my, 'linked');
					($ok, $message) = ($prog_ok, $prog_message);
					$hint = program_output_hint($ref, $my);
				}
			}
		}
		elsif ($mode eq 'link_program_t')
		{
			my ($prog_ok, $prog_message) = compare_program_outputs(
				"$testbase.$ref_suffix",
				"$testbase.$my_suffix",
				'linked',
				1);
			($ok, $message) = ($prog_ok, $prog_message);
			$hint = program_output_hint("$testbase.$ref_suffix", "$testbase.$my_suffix");
			if ($ok)
			{
				my ($inspect_ok, $inspect_message, $inspect_hint) =
					compare_optional_inspect("$testbase.$ref_suffix",
					                         "$testbase.$my_suffix",
					                         $host_tag);
				($ok, $message) = ($inspect_ok, $inspect_message);
				$hint = $inspect_hint if defined($inspect_hint);
			}
		}
		else
		{
			die "Unsupported compare_results mode $mode";
		}
	};
	if ($@)
	{
		$ok = 0;
		$message = "ERROR: " . $@;
		$message =~ s/\n+$//;
	}

	if ($ok)
	{
		++$npass;
		print "PASS\n\n" if $verbose;
		next;
	}

	if ($keep_going)
	{
		print "$display_test: $message\n";
		print $hint if $check_mode && $auto_check_keep_going && defined($hint);
		$failed = 1;
		next;
	}

	print fail_prefix();
	print "$test: $message\n\n";
	print rerun_hint();
	print $hint if defined($hint);
	print "TEST FAIL\n";
	exit(1);
}

if ($mode eq 'witness_t')
{
	if ($check_mode)
	{
		print compare_label() . ": " . ($failed ? "FAIL" : "PASS") .
			" ($npass/$witness_compared compared";
		print ", $witness_skipped skipped" if $witness_skipped != 0;
		print ")\n";
	}
	else
	{
		print "SUMMARY compared=$witness_compared failures=$witness_failures skipped=$witness_skipped\n";
	}
	append_keep_going_summary($repo_root, $cwd, $npass, $witness_compared, $failed)
		if $keep_going && !$check_mode;
	exit($failed && (!$keep_going || $auto_check_keep_going) ? 1 : 0);
}

if ($keep_going && $check_mode)
{
	print compare_label() . ": " . ($failed ? "FAIL" : "PASS") . " ($npass/$suite_total)\n";
}
elsif (!$keep_going)
{
	print compare_label() . ": PASS ($npass/$suite_total)\n";
}
append_keep_going_summary($repo_root, $cwd, $npass, $suite_total, $failed)
	if $keep_going && !$check_mode;
exit($failed && (!$keep_going || $auto_check_keep_going) ? 1 : 0);
