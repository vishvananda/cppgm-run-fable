# PA33 Audit

## Audit Plan

Scope: commits `a4e70ecd7..HEAD` (8 PA33 commits, ~1,930 insertions across
52 files), reviewed against `pa33/plan.md` and the assignment README. All
2917 tests through pa33 and the pa33 file audit pass at audit start; this
pass hunts cheating shapes, ownership problems, regression risks, and
performance costs rather than failures.

Files to inspect, by risk:

1. **-O1 inline-ctor expansion** — `lowering/lower_expand.cpp` (new, 442
   lines): the call-site expansion that removes the last reference to
   linkonce C1/C2 symbols. Highest cheat risk in the stage: verify the
   "simple ctor" predicate is semantic (literal member stores proven from
   the ctor body) and conservative (anything unproven keeps the call),
   not a pattern match on the fixture's class shape or symbol names, and
   that pruning still emits required symbols when a reference survives.
2. **Covariant return thunks** — `lowering/lower_vtable.cpp` (+137):
   verify the thunk adjusts the *returned* pointer with a real
   offset-from-class-layout computation (including null checks if the
   ABI requires them), synthesized per override fact, not a canned body;
   and that non-covariant thunk paths did not regress.
3. **Varargs surface** — `sema/sem_builtin.cpp`, `sema/sem_expr.cpp`,
   `lowering/lower_function.cpp`, `x86/lowir_to_mir_program.cpp` (+175),
   `x86/lowir_to_mir_flow.cpp`: verify the SysV register save area, the
   gp_offset/overflow_arg_area cursor updates, and `va_arg` both-path
   (register vs overflow) lowering are real for general argument counts,
   not shaped to the tested arity; `__builtin_alloca` must be a genuine
   dynamic stack adjust (alignment, value = rsp), not a fixed-size slot.
4. **Small-aggregate by-value SysV classification** — x86 call lowering:
   verify the two-INTEGER-eightbyte classification keys on layout facts
   (size, field classes) and is gated per the PA32
   `SeparateCompilation()` convention so pinned pa13–pa29 fixtures do
   not move; memory-path fallback must remain correct for shapes the
   fast path declines.
5. **Dependent-name/TT-param/lambda manglings** —
   `lowering/lower_name.cpp` (restructured), `lower_name_template.cpp`
   (+273), `lower_name_local.cpp` (new, 202), `lower_name_parts.h`:
   verify dependent `N…E` prefixes, `X <expr> E`, `sr` expressions,
   `T_IiE`/`I3boxE` TT-param spellings, and local/lambda substitution
   entries are fed by typed semantic facts (no demangled-text probing,
   no second name builder downstream), and the substitution-table order
   stays host-compatible.
6. **abi_tag / no_unique_address / __decay plumbing** — `ast/ast_parse_*`,
   `sema/sem_special.cpp`, `sema/class_info.cpp`,
   `sema/type_builder.cpp`: attributes must land as entity facts consumed
   by layout/synthesis/mangling; `__decay` must evaluate in the type
   system with alias transparency preserved, not string-rewritten.
7. **EH rethrow/cleanup and noexcept-terminate** —
   `lowering/lower_eh.cpp`, `x86/lowir_to_mir_eh.cpp`,
   `toolchain/runtime_library.cpp`: verify rethrow bookkeeping and
   non-matching-handler cleanup run through real region/action tables;
   the own-runtime `std::terminate` must be a real definition, not a
   link shim; no fallback-success paths.
8. **TLS dynamic-init order and the "unhoused operand" fix** —
   `lowering/lower_global.cpp`, `x86/lowir_to_mir_flow.cpp`,
   `x86/lowir_to_mir.h`: verify the ICE fix is structural (operand
   housing rule), not a special case. The plan documents a *known
   latent hazard*: the pool-exhausted TLS store stages through rax
   ahead of the wrapper call. Audit must resolve this — a documented
   wrong-code path is not an acceptable boundary.
9. **Vbase path dedup and overload/init fixes** — `sema/sem_apply.cpp`,
   `sem_class.cpp`, `sem_lambda.cpp`: virtual-base subobject dedup must
   be structural (one shared subobject fact), and the Guard<Fn>/aggregate
   fixes general, not test-shaped.

Performance risks to check:

- `lower_expand.cpp`: how expansion candidates are found — per-call
  rescans of the unit or program would be quadratic in functions/calls;
  the prune step must not re-walk all symbols per pruned ctor.
- `lower_vtable.cpp` thunk synthesis and `lower_transfer.cpp` (new):
  per-thunk or per-transfer scans over all program items.
- `x86/lowir_to_mir_program.cpp` (+175): varargs prologue/wrapper
  emission re-scanning module lists per function.
- `lower_name_template.cpp`: substitution-table growth — linear
  per-component lookups are acceptable, anything super-linear is not.
- Frontend attribute parsing: no per-token rescans in
  `ast_parser_core.cpp` attribute paths.

Ownership boundaries to check:

- Semantic facts (abi_tag, no_unique_address, TT-param kinds, dependent
  types, vbase paths) computed in `sema/` and carried on entities; the
  lower_name mangler consumes typed facts and stays the single name
  builder; object code preserves raw names.
- EH tables and thunks lowering-owned; x86 gains only ops LowIR cannot
  express (alloca, va register save).
- Host-only behaviors gate on `SeparateCompilation()`; whole-program
  fixture shapes must not move.

File-audit issues to check:

- The "audit splits" created `lower_name_local.cpp`,
  `lower_name_parts.h`, `lower_transfer.cpp` and moved code out of
  `lower_member.cpp`/`lower_name.cpp`/`lower_unit.cpp`. Verify splits
  are cohesive concern moves (not size-dodging), headers gained
  declarations only, nothing landed outside `--paths dev/src`, and no
  implementation hid in headers to dodge source-line caps.

## Findings

Four blockers, one latent-hazard family, and a set of cleanups. Every
blocker was reproduced against host g++ before fixing and re-verified
after; nothing was deferred.

1. **abi_tag leaked across declarations and duplicated under
   backtracking (blocker).** Tags accumulated in parser-global
   `last_abi_tags_`, which was neither part of tentative-parse
   save/restore nor cleared between declarations: a tag on an
   unrelated variable contaminated the next class's constructor
   (`_ZN1SC1B1vB1vB1vB1vEv` vs g++ `_ZN1SC1Ev`), and a leading-position
   tag quadruplicated through the member-form parse attempts. Adjacent
   string literals also spelled two tags instead of concatenating.
   Fixtures only used the safe trailing position.
2. **`[[no_unique_address]]` overlapped unconditionally (blocker).**
   Any empty nua member was placed at offset 0 with no same-type
   conflict check: `struct S { [[no_unique_address]] E a; E b; }` got
   sizeof 1 vs host's 2, violating the distinct-address rule for every
   sibling/base collision shape.
3. **Host EH unwind chains broke off the fixture paths (two blockers
   plus a leak).** (a) A noexcept function with any destructible local
   looped forever: the cleanup pad's bare `resume` re-entered the
   frame's cached phase-1 landing pad, never reaching the terminate
   dispatch. (b) A handler-local rethrow double-destroyed enclosing
   locals: both the inner wrap pad and the catch-cleanup pad ran
   `EmitCleanupsFrom(0)`. (c) An unmatched selector leaked
   enclosing-scope locals (`catch_next` resumed without running them)
   - matching the course reference but not host g++/15.2p1 - and the
   LSDA had no cleanup record to get phase 2 into the frame at all.
   Root causes: the catch pad's `eh_end` frame-record pop corrupted
   the host region dataflow (pads enter without their own region, so
   the pop dropped the *enclosing* region and uncovered the resume),
   and in-frame terminate routing relied on resume re-entry the host
   personality does not provide.
4. **Pool-exhausted TLS staging (the plan's documented hazard, plus
   two same-family latents).** The value staged in caller-saved rax
   was clobbered by the host-mode `_ZTW` wrapper call inside
   `MI_TLS_ADDR`; a frame-homed TLS load result never committed to its
   frame home; and `emit_tls_addr` never invalidated the rax alias
   despite being a call boundary. No fixture reaches pool exhaustion -
   these were latent wrong-code paths, not test failures.
5. **File-audit hygiene.** `AliasParamIndex` and
   `FundamentalKeywordCode` were parked in `lower_name_local.cpp`
   (nothing local-entity about them) to duck the 1500-line cap on
   `lower_name_template.cpp` (1489/1500) - both review agents flagged
   this as the clearest size-dodge signal. `ExprHasSideEffects` was
   duplicated into `lower_transfer.cpp` with the dead original left in
   `lower_member.cpp`; two file annotations pointed at the wrong file;
   `lower_expand.cpp`'s topic comment omitted a third of its content.
   The audit-split commits otherwise moved code verbatim (one bundled
   nested-lambda feature branch inside a "move" commit, noted).
6. **Sema acceptance gaps.** `__builtin_va_arg` accepted any operand
   as the cursor (an `int` silently miscompiled; gcc rejects); the
   alias-expansion fallback caught `std::exception` (so even
   `bad_alloc` degraded to a fallback spelling instead of
   propagating); the mangler never deduped repeated abi_tags.

Checked clean (no action): the -O1 ctor expansion is a semantic,
conservative, double-gated predicate over the real ctor body; covariant
thunks compute layout-fact offsets with null-checked result shifts
(virtual-base covariant paths reject loudly in sema, so lowering's
no-adjust fallback is unreachable); the varargs surface is a real SysV
implementation for general arity (save area seeded from named-arg
consumption, both register and overflow `va_arg` paths, genuine
dynamic-stack `alloca`, AL count); `gpr_pair` classification is
recursive layout-fact-driven, host-gated, and validator-enforced; the
mangler additions key on typed facts with structural substitution
bookkeeping (no fixture identifiers, no demangled-text probes, one
name-construction layer); no quadratic scans were introduced and the
PA32-fixed scan shapes were not reintroduced.

Accepted as fine, with reasons: `ClassFieldsAllInteger` recomputes per
call-site spelling (bounded by 16-byte classes - a handful of fields);
`LowerSimpleInlineConstruction` re-classifies per construction action
(non-conforming bodies exit at the first statement; conforming bodies
are by definition tiny); `Substitutions::Find` linear scans and the
transactional table copy per alias expansion (explicitly within the
plan's budget); the TT-param branch spells before checking `Find`
(idempotent on repeats, and the `AddParam` instance-refresh semantics
are pinned by checked g++ encodings, so reordering risks moving pinned
names for zero correctness gain); the value-vs-type re-reading of
nested dependent member template-ids (parameter kinds are unknowable
pre-instantiation); `lowir_to_mir_flow.cpp` sits at exactly its
1500-line cap (within limits; its next feature needs a split first).

## Changes Made

- `dev/src/x86/lowir_to_mir_inst.cpp`: `emit_tls_addr` invalidates the
  rax alias (TLS address materialization is a call boundary); the
  pool-exhausted TLS store spills its staged value to an anonymous
  frame slot across the address call and reloads after; the TLS load
  path commits frame-homed results via `commit_frame_result`. The
  pool-staged (callee-saved) paths emit byte-identical code to before.
- `dev/src/lowering/lower_eh.cpp`, `lower_member.cpp`,
  `lower_function.h`: `EmitUnwindLeave()` terminates in place under
  the armed noexcept region (inlined `exception ptr` + terminate
  trampoline call; a landing pad cannot double as a jump target) and
  resumes otherwise; the catch-cleanup pad's `eh_end` is emitted only
  on the whole-program path; pads beneath skipped catch contexts
  clean only the scope slice they own in host mode (the enclosing
  catch pad runs its own slice when the resume chains into it); the
  unmatched-selector route runs the enclosing cleanups in host mode;
  and a try dispatch publishes a bare `eh_cleanup` marker when that
  route has cleanup work so phase 2 enters the frame. Whole-program
  pad shapes are unchanged.
- `dev/src/ast/ast_parser.h`, `ast_parser_core.cpp`,
  `ast_parse_declarator.cpp`, `ast_parse_class.cpp`: abi_tag capture
  threads an out-param (`SkipDeclAdornments`/`SkipAttributeParens`)
  into the AST node being built - the declarator for trailing
  position, a per-attempt local spliced into the special-member
  declarator for leading position - and the parser-global bag is
  gone; adjacent literals concatenate per 2.14.5, commas separate
  tags. `dev/src/lowering/lower_unit.cpp` dedups the sorted spelling.
- `dev/src/sema/class_info.h/.cpp`: nua placement bumps past
  same-type empty subobjects (`nua_slots` probes, recursive base-tree
  probe, placed-field probe); later empty-member allocations bump
  past overlapped slots; `nua_extent` floors `nv_dsize` so bumped
  placements stay inside the object. All probes are inert for classes
  without nua members, so pre-PA33 layouts cannot move. `ClassInfo`'s
  init-list constructor became in-class initializers (the file's
  newer style; also returns the struct under the audit's block cap).
- `dev/src/sema/sem_expr.cpp`: `AnalyzeVaArg` requires the va_list
  cursor model (unsigned-long array or its decayed pointer) and
  rejects anything else through `OutsideBoundary`.
- `dev/src/lowering/lower_name_signature.cpp` (new): the
  function-template signature assembly (`TypeMentionsAliasSpec`,
  `MangleSignatureReturn`/`Parameters`,
  `MangleFunctionTemplateSpelled`) and the three public object-name
  entry points, moved verbatim from `lower_name_template.cpp`; the
  written-form services they consume are promoted to
  `lower_name_parts.h` declarations. `AliasParamIndex` and
  `FundamentalKeywordCode` moved home to `lower_name_template.cpp`
  from `lower_name_local.cpp`, which is now purely 5.1.7.
  Registered in `dev/frontend_source_sets.mk`.
- `dev/src/lowering/lower_name.cpp`: the alias-expansion fallback
  catches `std::runtime_error` (the boundary discipline) instead of
  `std::exception`.
- Cleanups: dead `ExprHasSideEffects` removed from
  `lower_member.cpp`; stale `(lower_member.cpp)` annotations in
  `lower_function.h` now point at `lower_transfer.cpp`;
  `lower_expand.cpp`'s topic comment covers its PA33 content.
- `pa33/plan.md`: the fixed TLS hazard left the boundary list; the
  Architecture Review and Final Architecture Review sections record
  the audited state.

## Validation

- Blocker repros vs host g++ (all in `/tmp/pa33audit/`): the
  noexcept-with-local shape exits 7 through the terminate handler
  (was: infinite unwind loop); handler-local rethrow destroys
  outer=1/inner=1 (was outer=2); unmatched catch destroys guard=1
  (was 0); nested rethrow prints the exact g++ event sequence
  including `outer-dtor` (was missing). abi_tag manglings are
  byte-identical to g++ for the leak, leading, multi-tag, and
  concatenation shapes; nua sizeof/offset facts match g++ on six
  shapes including base conflicts and member-before-nua.
  `__builtin_va_arg(i, int)` on an `int` now exits 1 (gcc rejects it
  too).
- `make -C pa33 test`: 74/74 after each fix group and after the
  signature split.
- `make test-report-through-pa33`: ALL TESTS PASSED (2917/2917) on
  the correctness fixes, and again after the mangler split - the
  host-gated EH/layout/staging changes move no pinned whole-program
  fixture.
- `perl scripts/cppgm_file_audit.pl --stage pa33 --paths dev/src`:
  pass with the six pre-existing `bad-division` warnings only.
  Post-split sizes: lower_name_template.cpp 1194, lower_name.cpp
  1447, lower_name_signature.cpp 361, lower_name_local.cpp 163,
  lower_name_parts.h 257.
