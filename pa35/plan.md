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

## Architecture Review

How the shipped implementation maps onto the ownership boundaries
above, from the loop-80 audit of the actual code:

- **Preprocessor**: GNU named-variadic parameters live in the macro
  layer as planned — `MacroDefinition` carries `named_variadic` beside
  `variadic`, `SameDefinition` compares both plus the parameter names,
  and `__VA_ARGS__` inside a named-variadic replacement list stays a
  definition-time error. The paste pass stays operand-driven
  (placemarkers), with nothing keyed on header or macro names.
- **Sema/templates**: the frontier fixes landed in their true owners.
  Injected-class-names (9p2/14.6.1p1) are synthesized lazily in
  `scope_lookup.cpp` and cached per scope, with sibling-specialization
  collapse decided from typed `spec_template` identity; 14.6.2p3
  dependent-base skipping is per-base links recorded by `sem_bases.cpp`
  at instantiation; 11.2 access contexts for instantiated out-of-class
  members are an RAII stack in `sem_template.cpp`; 14.7.2/14.7.3
  extern/explicit emission control is the typed `extern_suppressed`
  flag with symmetric set/clear. The one deliberate stringly artifact
  is the 14.5.7 alias-expanded return key (`template_spelling.cpp`):
  a last-resort conservative identity behind the typed comparisons,
  where an unrenderable form yields an empty key and claims nothing.
- **Failure handling**: tentative work rolls back typed. A failed
  class-specialization bind resets the entity/record so the next
  demand re-instantiates (and rethrows); overload probes catch only
  `NoViableOverloadError`; the hosted intrinsic-wrapper demotion is
  gated on typed faults (`UnimplementedBuiltinError`,
  `UnsupportedVectorForm`) rather than message prefixes, so genuine
  errors in wrapper bodies propagate (hardened in the audit).
- **Performance**: heavy-header cost was addressed with caches and
  representation (injected-name binding cached per scope, spelling
  keys bounded by a depth limit, specialization records keyed by
  canonical argument keys), not with limits or budget edits. The
  worst pa35 compile is 7.9s against the 45s harness budget.
- **Driver/object path**: unchanged contract — `-c` still lowers
  through the one LowIR route and writes a real relocatable object;
  the only lowering-side PA35 changes are the two new RTTI record
  kinds (now sized structurally off `ERttiVtableKind`) and local-class
  member mangling from typed scope facts.

## Final Architecture Review

Post-audit state (loop 80): the three blockers found — the gnu_inline
catch-all demotion, the dropped `vector_size` fact behind it, and the
string-matched member-specialization retry — are fixed at their
owners: typed faults in `sem_expr.h` thrown by name resolution, the
vector-typedef fact carried parser → `AstDecl` → `ScopeBinding`, and
14.7.3p18 replacement decided at the capture-merge site from the
`definition_instantiated` provenance flag with duplicates rejected.
Five course regression tests pin the fixed behaviors. File audit is
clean after splitting `AdoptFunctionTemplateDefinition` and
`BindMemberTemplateOfSpecialization` out of the two functions the
fixes had grown. No fixture gates, no hosted-only sema switches, no
test/harness edits, no timeout workarounds; `make
test-report-through-pa35` is green (3271/3271 including the new pins).
Remaining known debts, accepted deliberately: the 14.5.7 spelling key
(typed alias-expansion composition would replace it), and GNU vector
types are represented only as an outside-the-surface fact — real
vector-type support is future work for whichever stage first needs
the intrinsics to execute rather than demote.

## Status (complete)

77/77 pa35 tests pass; through-pa35 green with the file audit clean.
Beyond the loop-79 list (macros, parser tables, mangling, atomics,
enum widening, ctor/conversion fixes, rollback, friends), the final
frontier fixes were:

- Alias-transparent redeclaration identity (14.5.7,
  template_spelling.cpp): `_Require<...>` vs
  `typename enable_if<...>::type` spellings of std::swap's return
  merge through an alias-expanded canonical key.
- 11.2 access contexts for instantiated out-of-class member
  definitions (_Rb_tree's protected _Base_ptr in return types).
- 14.2p2: template-id member callees exclude non-template overloads
  (std::function's _M_access<T>() assignments).
- __function_type_info/__enum_type_info RTTI records (typeid through
  function-pointer pointees).
- Qualified operator-function member callees and template-id member
  qualifiers via ResolvePrefixScope (shared_ptr rvalue assignment).
- Paren-init pack expansion in variable initializers
  (deque::_M_insert_aux) and the __has_trivial_destructor trait.
- Body-less explicit specializations suppress local instantiation and
  reference the external symbol (14.7.3p6, basic_string's
  operator>> declaration).
- Constexpr specializations instantiate inside unevaluated operands
  (14.7.1p3, pair's enable_if constraint probes under decltype).
- A specialization's injected-class-name is its template's name
  (14.6.1p1, codecvt mem-inits), with sibling injected names of one
  template collapsing to the template (pa26 diamond).
