#pragma once

#include <cstdio>
#include <string>

// Reads the remainder of stdin as raw bytes. Tool entry points use
// this instead of streaming cin.rdbuf() into an ostringstream: with
// the default C-stdio synchronization that copy degrades to a
// character-at-a-time uflow/ungetc loop, which is a measurable share
// of tool runtime on the multi-megabyte stress tests. fread on the
// stdio FILE keeps the copy block-sized and also stays correct for
// the --batch-stdin runner, whose forked children inherit and reuse
// the process stdio state.
inline std::string ReadAllStdin()
{
	std::string bytes;
	char buffer[1 << 16];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0)
		bytes.append(buffer, got);
	return bytes;
}
