# PA35 Plan: Heavy Hosted Header Compiles (`cppgm++ -c`)

## Goal

Compile the heaviest hosted standard-library headers (`<vector>`,
`<unordered_map>`, `<tuple>`, `<random>`, `<functional>`, iostreams/string/
exception machinery) end to end with `cppgm++ -c` within a workable time and
memory budget. PA35 adds no new flags, no new object format, and no separate
hosted backend: the PA34 hosted environment plus the PA32/PA33 `-c` host-object
pipeline is the vehicle; PA35 raises the language/template/perf bar until the
heavy headers go through it cleanly.

Baseline at plan time: all 76 pa35 compile tests fail; everything through pa34
passes (3192/3268 report tests). First blocker: the preprocessor rejects GNU
named-variadic macro definitions (`#define __struct_group(TAG, NAME, ATTRS,
MEMBERS...)` in `/usr/include/linux/stddef.h`, reached through the pthread/
gthread include chain of `<bits/shared_ptr_base.h>` on GCC 15 headers).

## Ownership boundaries

1. **Preprocessor** (`dev/src/macro_table.{h,cpp}`, `dev/src/macro_expand.cpp`):
   hosted glibc/linux headers use GNU extensions the strict C++11 macro layer
   rejects. Add GNU named-variadic parameters (`args...`): the trailing named
   parameter absorbs the variadic tail, `__VA_ARGS__` stays invalid in such a
   definition, and definition identity (`SameDefinition`) includes the new
   flag. If the header set needs the GNU `, ## __VA_ARGS__` comma-deletion
   extension, implement it in the paste pass, keyed off the paste operands
   (comma left, empty variadic right), not off header names.
2. **Sema / templates** (`dev/src/sema/*`): the heavy headers stress
   instantiation depth, partial-specialization selection, SFINAE probes, and
   large overload sets far harder than PA34's anchors. Missing language rules
   discovered here get fixed in their true owner (binder, deduction,
   instantiation, constexpr), never behind hosted-only switches. Semantic
   facts come from typed compiler state (Type/Decl/Scope), not from formatted
   text.
3. **Performance** (same sema files + caches that already exist for
   specializations): the productive failures at this tier are perf cliffs —
   re-resolving the same bound pack, re-instantiating the same specialization,
   re-evaluating the same trait. Prefer memoizing repeated resolution keyed on
   canonical types/args over deepening recursion guards or raising limits.
   Perf work must keep the single LowIR lowering route used by
   `--emit-lowir`; no hosted-only backend shortcut.
4. **Driver/object path** (`dev/cppgm++.cpp`, `dev/src/x86/*`): unchanged
   contract from PA32-PA34 — `-c` writes a host-linker-compatible relocatable
   object. PA35 only requires that the compile succeeds; emitted-code
   correctness at run time is PA36's contract, but lowering must still be the
   real pipeline (no dummy/empty objects).

## Method

Work the failing tests as a frontier: each failure names a real header
construct we do not yet accept or a perf cliff. For each:

- minimize to the offending header + construct (host `g++ -H` include chain,
  bisect with direct `#include` of inner headers);
- fix in the owning stage's code, with the standard/GNU documentation as the
  contract, not the test text;
- re-run the scoped report (`make test-report ACTIVE_TEST_REPORT_PAS='pa35'`)
  for the frontier, and the full `make test-report-through-pa35` after any
  parser/sema/lowering/shared change to catch regressions in earlier stages.

Timeouts on heavy headers are treated as perf bugs in our compiler (memoize,
de-duplicate work), never as harness/test budget problems.

## Validation

- `make test-report-through-pa35` clean is the exit criterion (also guards
  pa1..pa34 against regressions from shared-code changes).
- `perl scripts/cppgm_file_audit.pl --stage pa35 --paths dev/src` must stay
  clean; split files that grow past the audit ceiling along ownership lines.
- New regression tests for language bugs found here go under
  `cppgm.tests/course/paN/` of the owning stage when they are not
  header-specific; header-pressure cases stay in `pa35/tests/`.
