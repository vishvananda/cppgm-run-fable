# PA35 Audit

Loop-80 audit of the PA35 hosted heavy-header compile work
(`e5d1e780d..HEAD`, 33 commits, ~3100 insertions across 57 files,
concentrated in `dev/src/sema/`). Baseline at audit start: through-pa35
green (3266/3266 in the report log), `cppgm_file_audit.pl --stage pa35`
exit 0, git clean.

## Audit Plan

### Files to inspect

- `dev/src/sema/template_spelling.cpp` (new, 437 lines) and its caller
  `dev/src/sema/template_order.cpp:573-646` — 14.5.7 alias-transparent
  redeclaration identity computed from *rendered strings*.
- `dev/src/sema/template_arg_const.cpp`, `sem_var_template.cpp`,
  `decl_enum.cpp`, `sem_access.cpp`, `sem_binder_state.h` (new) plus the
  shrunk donors `template_args.cpp`, `sem_spec.cpp`, `decl_binder.cpp`,
  `sem_class.cpp` — the loop-79 file-audit splits (commit d0a875bf5).
- Every `catch` added in the range: `sem_binder.cpp`, `sem_ctor.cpp`,
  `sem_member.cpp`, `sem_member_body.cpp`, `sem_apply.cpp`,
  `sem_spec.cpp`, `sem_template.cpp`, `sem_var_template.cpp`,
  `template_arg_const.cpp`, `template_args.cpp`, `sem_call.cpp`,
  `sem_new.cpp`, `sem_operator.cpp`.
- `dev/src/lowir/lowir_validate.cpp` — a validator diff landed during a
  "make it compile" push; must not be a weakened check.
- `dev/src/lowering/lower_vtable.cpp` — the ERttiVtableKind name-cache
  out-of-bounds fix (1b2dccb38); verify the sizing is now structural,
  not another magic number.
- `dev/src/sema/scope_lookup.cpp`, `sem_bases.cpp` —
  injected-class-name synthesis at lookup (f52ba1015, 2a3dafeb9,
  f2edf29f3), including the pa26-facing sibling-collapse change.
- `dev/src/macro_table.{h,cpp}`, `macro_expand.{h,cpp}` — GNU
  named-variadic macros; `SameDefinition` must include the new flag.
- `dev/src/sema/sem_hosted_builtin.cpp`, `lowering/lower_builtin.cpp` —
  the `__atomic_*` ladder; name-dispatch is expected here, but check
  the two files agree on signatures.
- `pa35/tests/`, `pa35/scripts/`, `pa35/Makefile` — confirm handout
  harness/tests are unmodified and anchors are real.

### Performance risks to inspect

- Injected-class-name synthesis on the lookup path: per-lookup work or
  allocation in hot scope walks.
- `template_spelling.cpp` renderer: string building per redeclaration
  check; confirm it only runs after the typed `TypeEquals` path fails.
- Instantiation rollback (68cba6f86): full-state copies per tentative
  instantiation would be a hot-path copy hazard.
- Wall-clock headroom of the pa35 suite itself (timeout flakes on a
  loaded box are perf headroom per project memory).

### Ownership boundaries to inspect

- No hosted-only semantic switches in sema (plan boundary 2): grep the
  range for hosted/SeparateCompilation gates added to `dev/src/sema/`.
- Single LowIR lowering route preserved (plan boundary 3/4): no
  hosted-only backend shortcut; `-c` still writes a real object.
- Language fixes in their true owner: injected-class-name (9p2) in
  lookup/scope, access (11.2) in sem_access, deduction in
  template_order/template_args — spot-check the frontier fixes landed
  in the owning stage's files rather than pa35-specific guards.
- Stringly facts: the 14.5.7 spelling key is text by design; decide
  whether its use is bounded to a conservative last resort (acceptable
  documented exception) or leaks into ownership of typed facts.

### File-audit issues to inspect

- `cppgm_file_audit.pl --stage pa35 --paths dev/src` warnings
  (bad-division on `parser.h`, `class_info.h`, `sem_binder.h`,
  `sem_expr.h`; shared-stem note) — confirm they predate PA35 or are
  justified, and that PA35 did not grow header-implementation bodies
  to dodge the .cpp ceiling.
- The d0a875bf5 splits: cohesive module boundaries vs mechanical
  overflow relief; no duplicated fragments left in donors; no
  implementation moved to paths the audit does not walk
  (`frontend_source_sets.mk` edit must list the new files).

### Cheating patterns to rule out

- Fixture- or header-name-conditioned acceptance anywhere in
  `dev/src/` (grep for pa35 test names, `bits/`, header basenames).
- Fallback success: any added catch that converts a real error into
  silent acceptance rather than rolling back a tentative probe.
- Dummy artifacts: pa35 harness must compare exit status *and* run the
  real `-c` object emission; anchors (`static_assert`/`decltype`)
  must be present in the tests.
- Regression masking: pa26 sibling-injected-name collapse and enum
  widening touched earlier-stage behavior; through-pa35 green is the
  guard, plus a direct read of both changes.

## Findings

Ordered by severity. Every blocker was fixed in this audit pass (see
Changes Made); the accepted-as-is items carry their rationale.

### Blockers (fixed)

1. **gnu_inline demotion swallowed every body error**
   (`dev/src/sema/sem_binder.cpp`, `BindFunctionBody`). The hosted
   intrinsic-wrapper concession caught `const std::exception&` and
   demoted the definition to a declaration whenever the function was
   `__attribute__((gnu_inline))` — masking *any* genuine semantic
   error in such a body (undeclared names, bad conversions, overload
   failures all compiled clean, surfacing later as a spurious
   undefined reference at best). The non-gnu_inline arm keyed on the
   error *message prefix* `"undeclared name __builtin_ia32_"` — a
   stringly gate. Fixed by typing the outside-the-surface faults:
   `UnimplementedBuiltinError` (an undeclared reserved `__builtin_*`
   name, thrown at the two name-resolution sites) and
   `UnsupportedVectorForm` (below); only those demote, and only under
   the original gnu_inline/ia32 gates. Regression tests pin both
   directions.
2. **GNU `vector_size` fact silently dropped**
   (`dev/src/ast/*`, `dev/src/sema/decl_binder.cpp`, `scope.h`).
   `typedef float __m128 __attribute__((vector_size(16)))` bound
   `__m128` as plain `float`; wrapper bodies over vector literals
   (`(__m128){0,0,0,0}` in `_mm_setzero_ps`) then failed with
   arbitrary scalar errors — which is exactly what the blanket
   swallow above was hiding. The parser now records the attribute
   (`AstDecl::vector_size`, mirroring the `gnu_inline` capture), the
   typedef binding carries it (`ScopeBinding::vector_spelled`), and an
   expression-position use of such a name throws the typed
   `UnsupportedVectorForm`, which demotes gnu_inline wrappers instead
   of miscompiling the literal as a scalar cast. The fact is typed
   compiler state end to end, not a header- or name-conditioned gate.
3. **Member-of-specialization retry swallowed ODR redefinitions**
   (`dev/src/sema/sem_spec.cpp`, `BindExplicitSpecialization`
   DK_TEMPLATE). The 14.7.3p18 replacement was implemented as: catch
   any exception whose message starts with `"redefinition of function
   template"`, clear `has_definition` on *every* same-name member
   template, retry. A second explicit `template<> template<...>`
   definition of the same member was silently accepted (genuine ODR
   violation), and unrelated overloads' instantiated definitions could
   be wiped. Fixed with a typed provenance flag
   (`TemplateInfo::definition_instantiated`, set where a definition is
   captured under `instantiating_`): the merge site itself now permits
   replacement only of a pattern-instantiated definition when the
   capture is the explicit member-of-spec path
   (`replace_instantiated`), with no exception round-trip and no
   over-clearing. Regression tests pin both the rejection and the
   legitimate replacement.
4. **RTTI vtable name caches were three independent magic `7`s**
   (`dev/src/lowering/lower_program.h`, `lower_vtable.cpp`). The
   loop-79 out-of-bounds segfault was fixed by hand-bumping `[5]` to
   `[7]` in three places; the next `ERttiVtableKind` addition would
   reintroduce the overflow. Now `RTTI_VT_KIND_COUNT` sizes the member
   cache and `static_assert`s pin both string tables to the enum.

### Accepted as-is (with rationale)

- **14.5.7 alias-expanded return key is a rendered string**
  (`template_spelling.cpp`, used at `template_order.cpp:640-645`).
  Stringly by design and documented as such: it runs only after the
  typed `TypeEquals`/pattern comparisons fail, both sides render under
  the same lookup scope with parameters positionalized, any form
  outside the renderer's subset (or past the depth limit) yields an
  empty key, and empty keys claim no identity (`!left.empty() && left
  == right`). False positives require two different types with
  identical rendered spellings under one scope — which is the 14.5.7
  equivalence itself. Composing alias-expanded types abstractly would
  be the typed ideal but is a representation change out of scope here;
  the conservative-failure direction (distinct templates overload
  rather than merge) is the safe one.
- **All other added catch sites** (24 audited across sema): overload
  probes rethrowing `NoViableOverloadError` with context, scope-restore
  rethrows, SFINAE/deduction probes with defined false-fallthroughs,
  the class-instantiation rollback (resets the hollow record, then
  rethrows), and byte-identical moved code from the file-audit splits.
  None swallow a real error.
- **Atomic builtin name ladders** (`sem_hosted_builtin.cpp`,
  `lower_builtin.cpp`): name-keyed dispatch is the identity of
  builtins; the two sides were cross-checked for signature/lowering
  agreement (including `__atomic_compare_exchange` vs `_n` pointer
  forms) — consistent, no stubs.
- **File-audit warnings** (bad-division on `parser.h`,
  `class_info.h`, `sem_binder.h`, `sem_expr.h`; `sem_` shared-stem
  note): pre-date PA35; PA35 shrank `sem_binder.h`'s implementation
  content (state structs moved to `sem_binder_state.h`).

### Verified clean

- File-audit splits (d0a875bf5) are faithful: donors emptied, new
  files in `frontend_source_sets.mk` and walked by the audit,
  moved bodies byte-identical, linkage adjustments only.
- pa35 harness, tests, and refs are handout-intact; no commits touch
  `pa35/tests`, `pa35/scripts`, `pa35/Makefile`, or earlier-stage
  refs in the range.
- No fixture-, test-, or header-name-conditioned acceptance anywhere
  in `dev/src`; no `getenv` sniffing; no hosted-only semantic switch
  added in sema; header-path literals confined to the hosted include
  probing files where they belong.
- `lowir_validate.cpp` change is diagnostics-only (duplicate-alias
  message now lists the conflicting targets); no check weakened.
- Single lowering route preserved: pa35 tests emit real relocatable
  objects through the same LowIR path (spot-checked a 283KB object
  from a heavy header compile).
- Injected-class-name synthesis (9p2/14.6.1p1) is lazily cached per
  scope (one name compare per miss); per-base dependent-base links are
  a small vector scanned per base — no lookup-path regression.
- Perf headroom: worst pa35 test compiles in 7.9s unloaded
  (600-regex-iterator-difference-alias) against the 45s harness
  budget; the rest are ≤3.5s. No timeout-budget edits anywhere.

## Changes Made

- `dev/src/sema/sem_expr.h`: typed `UnimplementedBuiltinError` (carries
  the builtin name) and `UnsupportedVectorForm` faults.
- `dev/src/sema/sem_binder.cpp`: the two `undeclared name` throw sites
  raise the typed builtin fault for reserved `__builtin_*` names
  (`ResolveValue`, and `sem_operator.cpp`'s ADL-call site);
  `BindFunctionBody`'s demotion catch narrowed to
  `WrapperBodyDemotes()` (typed faults only, same gnu_inline/ia32
  gates); `RejectVectorSpelledType` probes cast/callee type names.
- `dev/src/ast/ast.h`, `ast_parser.h`, `ast_parser_core.cpp`,
  `ast_parse_decl.cpp`: `vector_size` attribute recorded onto simple
  declarations (the `gnu_inline` capture pattern).
- `dev/src/sema/scope.h`, `decl_binder.{h,cpp}`:
  `ScopeBinding::vector_spelled` set from the declaration fact at
  typedef binding (redeclarations accumulate it).
- `dev/src/sema/template_info.h`, `sem_spec.cpp`,
  `sem_member_template.cpp`, `sem_binder.h`:
  `definition_instantiated` provenance flag; `replace_instantiated`
  threaded from the explicit member-of-spec path to the merge site;
  the string-matched clear-and-retry deleted.
- `dev/src/lowering/lower_program.h`, `lower_vtable.cpp`:
  `RTTI_VT_KIND_COUNT` sizes the name cache; `static_assert`s pin the
  tail/object tables to the enum.
- File-audit conformance for the above: `AdoptFunctionTemplateDefinition`
  and `BindMemberTemplateOfSpecialization` extracted from the two
  functions the fixes had pushed past the 120-line ceiling.
- Debug leftovers removed: unused `<cstdio>/<cstdlib>` includes and a
  bypassed `using std::to_string` (sem_apply, sem_binder, sem_ctor,
  sem_member), a `(void)0;` no-op (sem_bases), the unused `dest_bool`
  local (sem_convert).
- Regression tests added under `cppgm.tests/course/pa35/compile/`
  (runs via the pa35 course bucket): gnu_inline body error propagates
  (FAIL pin), unimplemented-intrinsic and vector-typedef wrappers
  demote (SUCCESS pins), duplicate member-spec definition rejected
  (FAIL pin), 14.7.3p18 replacement accepted (SUCCESS pin).

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa35 --paths dev/src`:
  exit 0 (8 pre-existing warnings, 0 fatal).
- `make -C pa35 test`: 77/77 handout + 5/5 new course tests.
- `make test-report-through-pa35`: all tests pass (3271/3271 — the
  3266 baseline plus the five new course pins), pa1..pa35 all green —
  no earlier-stage regressions from the sema/parser/lowering changes.
- Direct probes (compiled with `dev/cppgm++ -c`): gnu_inline body with
  a genuine undeclared name rejects; gnu_inline ia32/vector wrappers
  demote and compile; non-gnu_inline non-ia32 unimplemented builtin
  rejects; duplicate explicit member definitions reject; legitimate
  pattern-replacement compiles.
