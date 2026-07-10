# PA24 Audit

## Audit Plan

Scope: the PA24 commits (`d95a829fd..e949c9ab0` plus the earlier
`9b1806856` cast/braced-init cluster) against `pa24/plan.md`, the PA24
README boundary, and the through-pa24 suite (green at audit start:
`make test-report-through-pa24` reports 2373/2373 — the outer harness
counts the same run as 2387 with its per-stage extras; file audit
clean).

Files to inspect (the PA24-changed surface):

- **New sema units**: `dev/src/sema/sem_auto.cpp` (auto deduction +
  range-for desugar), `dev/src/sema/sem_lambda.cpp` (closure-class +
  internal-function hybrid), `dev/src/sema/decl_using.cpp` (split from
  decl_binder), `dev/src/sema/sem_convert.cpp`/`sem_apply.cpp` additions.
- **Shared sema touched**: `sem_binder.cpp/.h`, `sem_expr.cpp/.h`,
  `sem_operator.cpp`, `sem_cast.cpp`, `sem_class.cpp`, `class_info.cpp`,
  `sem_member_body.cpp`, `sem_lifetime.cpp`, `template_body.cpp`,
  `template_deduce.cpp`, `template_order.cpp`, `type.cpp`,
  `type_builder.cpp`.
- **Lowering touched**: `lower_expr.cpp`, `lower_function.cpp/.h`,
  `lower_member.cpp`, `lower_name_template.cpp`, `lower_unit.cpp`.
- **Parser touched**: `ast_parse_stmt.cpp` (range-for syntax).

What I will check, per audit charter:

1. **Cheating / substitutes**: no test-name or source-shape acceptance
   gates; no dummy/templated LowIR emission; lambdas and range-for must
   go through real class synthesis / real desugaring, not pattern-matched
   output; the internal free function for captureless lambdas must hold a
   real bound body, not a copied payload. Verify the comparator scripts
   and `.ref` handling were not weakened and `.my` outputs are freshly
   generated.
2. **Regressions**: earlier-stage suites all pass (24/24 stages); confirm
   monotonic-extension rule — PA23-subset programs must not change shape
   (spot-check via full suite, which reuses earlier stages' refs).
3. **Ownership boundaries** (plan §Ownership): AST owns syntax only —
   range-for fields must carry no semantic decisions; sema owns deduction
   and desugaring — downstream must never see `auto` or undesugared
   range-for; lowering must gain no new IR concepts. Check for stringly
   semantic facts (name-string comparisons standing in for typed facts,
   e.g. `__range`/`operator()` string matching in lowering) and for
   downstream re-derivation of facts sema already owns (e.g. lowering
   re-deducing lambda pointer-conversion targets).
4. **Performance**: capture-rewrite hooks (`TryCaptureUse`,
   `ThisValueNode`) run on every id-expression — check they are gated
   and don't walk all frames/scopes per use; per-(class, arity) aggregate
   helper keying must not linear-scan all classes; range-for desugar and
   auto deduction must not re-bind or re-walk bodies; look for repeated
   full-`Program` walks added to hot paths.
5. **File-audit integrity**: the `sem_auto.cpp`/`decl_using.cpp` splits
   must be genuine unit splits (no hidden fragments, no code moved to
   paths the audit does not check); `perl scripts/cppgm_file_audit.pl
   --stage pa24 --paths dev/src` must pass unmodified.

Method: read `sem_auto.cpp` and `sem_lambda.cpp` in full; targeted reads
of the shared-file diffs; parallel scans for stringly gates, quadratic
walks, and harness weakening; fix every blocker found before returning.

## Findings

Numbered by severity; every blocker was fixed in this pass.

1. **Duplicated layout ownership in closure construction (fixed).**
   `LowerClosureInit` (dev/src/lowering/lower_member.cpp) computed each
   capture field's address as `index i8 <dest>, i * 8`, re-deriving the
   closure layout positionally, while sema already records the
   authoritative offsets when `EnsureCaptureField`/`EnsureThisField`
   place fields through the shared `LayoutField` machinery (the
   capture-*read* path already consumes those via `member_offset`).
   The two agreed only because every capture field is an 8-byte
   pointer; any padding or field change would silently diverge.
2. **Range-for plain-identifier shadowing (fixed).** To match the
   reference's `__range`-free shape, `BindRangeForStatement` reuses a
   plain-identifier range expression directly. Because `BindVariable`
   adds the loop variable's binding before analyzing its initializer,
   `for (int a : a)` resolved the synthesized `a[__idx1]` against the
   just-declared loop variable — a hard error on valid C++ (g++
   compiles it; 6.5.4p1 evaluates range-init outside the loop-variable
   scope via `__range`).
3. **Auto deduction does not reuse `template_deduce.cpp` (reviewed,
   accepted).** `MatchAutoPattern`/`DeduceAutoDeclared` spell the
   14.8.2.1-subset rules (decay, cv drop, forwarding-reference)
   directly. Assessed as rule-level overlap, not fact-ownership
   duplication: the deduced type has exactly one owner
   (`DeduceAutoDeclared`, shared by variables, returns, member
   returns, lambdas, and range-for). Routing through
   `DeduceFixedParameter` would require inventing a `TK_TYPE_PARAM`
   with a bound-slot vector, substituting back, and still carving out
   the namespace-scope function-view rule (a function-typed
   initializer must *not* decay there — the reference lambda model)
   which `DecayForDeduction` cannot express. The adapter would exceed
   the ~50 shared lines and couple two stable units; kept separate.
4. **Reference-pinned shape accommodations (verified against refs, not
   test-specific).** (a) Postfix `++`/`--` over a conversion-call
   lvalue re-lowers the call for the store address — the checked-in
   ref (`200-conversion-operator-reference-incdec.ref`) shows two
   `operatorunsigned_` calls for the postfix form, so this is the
   pinned contract, generalized by node kind (`SN_CALL_EXPRESSION`),
   not by fixture. (b) A move over an enum-membered class keeps the
   synthesized constructor call (`MakeConstructorCall`), keyed on a
   typed class property. (c) Captureless lambdas keep the closure
   object view under cv-`auto` or local-type-owning bodies
   (`closure_object_view_`), keyed on the class entity.
5. **Hybrid captureless model cost (accepted, documented).** A
   captureless lambda binds its body twice (internal function + closure
   `operator()`), a bounded 2x per captureless lambda; the synthesis is
   cached per (lambda AST, enclosing body) and both forms emit only on
   demand through the deferred weak-flag sweep (`RegisterDeferred`
   marks the function internal; unused forms never render).
6. **Scans clean.** Harness: pa24 comparator/Makefile byte-identical to
   pa22/pa23 apart from the stage root; no test-name/fixture gates, no
   `getenv` gating, no canned LowIR strings, no `.ref` reads, and the
   tested binary builds from `dev/src` (the reference binary is only
   used by the separate `ref-test` maintenance target). Performance:
   `TryCaptureUse`/`ThisValueNode`/`CurrentThisType` are O(1) outside
   lambda bodies (empty frame stack) and capture-count bounded inside;
   the per-(class, arity) aggregate constructor is memoized
   (`aggregate_ctor_covers`); no full-unit walks, quadratic loops,
   erase-in-loop, timeouts, or iteration bail-outs were added; the
   `MakeConversionSource` list copy is built once per argument and
   reused across candidates. Ownership: no stringly semantic facts
   (hidden names are generated, never compared); the AST range-for
   fields carry syntax only; `SN_CLOSURE_INIT` adds no LowIR concept.
   Minor accepted cost: the enum-member scan in `MakeConstructorCall`
   is O(fields) per implicit copy/move call.

## Changes Made

- `dev/src/lowering/lower_member.cpp`: `LowerClosureInit` now looks up
  the closure class record and indexes each store by the recorded
  field offset (child i pairs with field i, capture order); throws if
  the record is missing rather than guessing.
- `dev/src/lowering/lower_program.h`: `ProgramClass` moved to the
  public query surface for that consumer.
- `dev/src/sema/sem_auto.cpp`: `BindRangeForStatement` computes the
  declared loop name up front; the plain-identifier shortcut applies
  only when the loop declaration does not shadow the range identifier,
  otherwise the standard hidden `__range` binding is used. No pinned
  ref shape changes (the fallback triggers only on the collision that
  previously failed to compile).
- `dev/src/sema/sem_binder.cpp`: stray double blank line removed.
- `pa24/plan.md`: added `Architecture Review` and `Final Architecture
  Review`.

## Validation

- Shadowing fix: `for (int a : a)` over `int a[3]` now compiles,
  desugars through `$__range1`, and the produced LowIR executes to
  exit 0 through `reference-binaries/lowir2native`, matching the g++
  oracle on the same source. Non-colliding loops keep their previous
  shape (the shortcut condition only gained the collision guard);
  closure construction keeps byte-identical output today (all capture
  fields are 8-byte pointers at packed offsets) while now consuming
  sema's recorded offsets.
- `pa24: make test` — 94/94 pass.
- `make test-report-through-pa24` — 2373/2373, all 24 stages pass.
- `perl scripts/cppgm_file_audit.pl --stage pa24 --paths dev/src` —
  pass (3 pre-existing header-division warnings, unchanged by PA24).
