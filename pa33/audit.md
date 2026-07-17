# PA33 Audit

## Audit Plan

Scope: commits `a4e70ecd7..HEAD` (8 PA33 commits, ~1,930 insertions across
52 files), reviewed against `pa33/plan.md` and the assignment README. All
2919 tests through pa33 and the pa33 file audit pass at audit start; this
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
