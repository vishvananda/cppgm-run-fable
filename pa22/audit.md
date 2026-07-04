# PA22 Audit

## Audit Plan

Scope: the 41 pa22 commits `2eb173d77..59ed6b028` (3,761 insertions across
48 files under `dev/`), reviewed against `pa22/plan.md`, `pa22/README.md`,
and the through-pa22 gate (1922/1922 green at audit start).

### Files to inspect

- `dev/src/sema/template_args.cpp` / `dev/src/sema/const_expr.{cpp,h}` /
  `dev/src/sema/sem_binder.h` — the `__is_implicitly_constructible`
  "tuple-constraints gate" intrinsic (commit 59ed6b028). Highest-risk item:
  a name-keyed constant-evaluation shortcut that skips the function body.
  Verify against `pa22/cppgm++-ref` that this is genuine reference-dialect
  behavior (an implementation-reserved-identifier intrinsic) and not an
  over-fit acceptance gate for one fixture; verify the structural guards
  (class-template specialization, leading bool value argument, member
  function template of that name) match the reference's actual rule.
- `dev/src/sema/sem_expr.{cpp,h}`, `template_args.cpp`
  (`TryConstantClassBool`) — "integral_constant-style" conditional folding
  (8eb6327f1). Verify the fold is keyed on structure (constant contextual
  bool conversion), not on library names, and cannot change the value of a
  conditional whose branches have side effects.
- `dev/src/sema/sem_ctor.cpp`, `sem_lifetime.cpp`, `lowering/lower_member.cpp`
  — effect-free default-construction elision (89b82e152) and pack-deduced
  delegating temporaries (a434bceb9). Verify elision is gated on
  effect-freeness derived from semantics, not on test shapes, and that
  odr-use/instantiation side effects are preserved.
- `dev/src/lowering/lower_expr.cpp` — pointer-arithmetic count widening
  (7c8f6b3d0) and one-byte pointer-difference scale skip (24765b6fc).
  Verify these are type-driven lowering rules, not fixture-driven output
  tweaks that would produce wrong code for other operand shapes.
- `dev/src/sema/sem_convert.{cpp,h}` — base-less user-copy classes pass by
  address (2a97c9371). Verify the ABI classification is a class-property
  rule, not keyed to specific tests.
- `dev/src/lowering/lower_name_template.cpp` (new, 588 lines),
  `lower_name_parts.h` (new header), `lower_name.cpp` — dependent-signature
  Itanium mangling (task 9). Check: written-form fallback never silently
  produces wrong manglings for supported shapes; the stringly substitution
  keys (`TP:n`, `T:name<...>`) stay contained inside the mangler; the new
  header is covered by the file audit (no code moved to unchecked paths).
- `dev/src/sema/template_deduce.cpp` (719 lines changed) — SFINAE
  candidate dropping, cache hygiene on failure, overload-set arguments.
  Perf risk: failed-probe re-deduction on every identical call; repeated
  full candidate re-substitution.
- `dev/src/sema/template_order.cpp` (new, 501 lines) — partial ordering.
  Perf risk: pairwise ordering is inherently quadratic in candidates;
  check the transformed-type walk is not recomputed per comparison pair
  more than necessary; check the daab77d2e split is a real unit.
- `dev/src/sema/sem_spec.cpp`, `sem_member_template.cpp`, `template_body.cpp`,
  `scope_lookup.cpp` — void_t deferral/re-substitution, decl-order lookup
  stamps, per-instantiation friends. Check decl-order stamps are integers
  on bindings (not string comparisons) and lookup does not rescan whole
  scopes per name.
- `dev/src/lowering/lower_unit.cpp` — unconditional weak emission of
  out-of-class inline special members (25c35d503). Check this does not
  regress earlier PAs' emission sets (through-gate covers this) and does
  not emit unreferenced strong symbols.

### Performance risks to check

- Deduction probe caching: failed `EnsureFunctionSpecialization` probes must
  not poison the cache, but repeated identical failing probes should not
  redo full class-body instantiation each time.
- Partial ordering: candidate-pair loops and per-pair transformed-type
  construction.
- Decl-order-aware lookup (14.6.4 subset): per-lookup scans vs stamps.
- `FindOwnBinding`/scope scans added by the intrinsic and friend work on
  hot paths.
- Repeated whole-suite walks: none expected in the compiler proper; verify
  no per-call linear scans over all specializations were added
  (`template_info.cpp` +89).

### Ownership boundaries to verify

- Deduction/unification in `template_deduce.cpp`; argument resolution in
  `template_args.cpp`; overload resolution consumes candidates only;
  instantiation timing in `sem_template.cpp`; mangling in `lower_name*`.
- No stringly semantic facts crossing module boundaries: template
  identity, pack slots, and deduction bindings should be typed structures;
  mangler-internal substitution keys are presentation-local by design.
- No downstream recovery in lowering of facts sema already knows
  (e.g. lowering re-deriving effect-freeness or ABI class by re-scanning
  the AST when sema has the class summary).

### File-audit issues to check

- `perl scripts/cppgm_file_audit.pl --stage pa22 --paths dev/src` passes.
- New header `lower_name_parts.h` and grown headers (`sem_binder.h` +100)
  hold declarations/small inlines only — no implementation moved to
  headers to dodge per-file caps.
- Helper extractions for the function-size audit (59ed6b028 in
  `lower_member.cpp`, daab77d2e splits) are cohesive factorings, not
  fragment-hiding.

## Findings

### Verified acceptable (with evidence)

1. **Tuple-constraints gate intrinsic (59ed6b028) — genuine reference
   dialect, correctly guarded.** Probed `pa22/cppgm++-ref` directly:
   with gate=false and a body reading `return true;` the reference still
   selects the varargs overload (gate wins, body ignored); renaming the
   member by one character (`__is_implicitly_constructiblez`) makes the
   reference evaluate the body normally; a plain-class qualifier also
   evaluates the body. So the reference really does treat the exact
   implementation-reserved name `__is_implicitly_constructible`, called
   with zero arguments through a class-template-specialization qualifier
   with a leading bool value argument, as an intrinsic returning that
   gate. The implementation keys on exactly that structure (no test
   names, no `_TCC` spelling), and user programs cannot legitimately own
   a `__`-prefixed member name (17.6.4.3.2).
2. **integral_constant-style conditional fold (8eb6327f1) — structural
   and reference-faithful.** The fold keys on class structure (single
   `return <name>;` conversion operator over a static constant, both
   branches literal), not library names. A probe with a side-effectful
   condition (`f() ? 0 : 1` where `f` writes a global) shows the
   reference itself drops the whole condition evaluation; our fold
   matches the oracle exactly, including that behavior.
3. **Constructor-call elisions (89b82e152, a434bceb9) — provably no-op.**
   Default-construction elision is gated on a cached class-graph
   effect-freeness fact and keeps the callee odr-used; the pack-deduced
   delegating-temporary drop additionally requires
   `!has_user_ctor && !NeedsConstruction`, so no elided call could have
   observable effects.
4. **By-address classification (2a97c9371)** is a class-property rule
   forced by the union of pa16 (with-base direct) and pa22 (base-less
   indirect) fixtures. **Pointer-count widening (7c8f6b3d0)** and the
   **one-byte difference scale skip (24765b6fc)** are type-driven and
   5.7-correct. **Unconditional weak emission of spelled-inline
   out-of-class special members (25c35d503)** is a 7.1.2p4 linkage rule.
5. **Dependent-signature mangler (task 9)** — reviewed for fallback
   totality (single catch, all state local, fallback rebuilds from a
   fresh substitution table; no truncated output possible), key
   containment (`TP:`/`T:`/`Dp|`/... keys never escape the two mangler
   translation units), and caching (one mangling per emitted symbol via
   `function_index_`). The broad fallback `catch (std::exception)` can
   mask a bug in the written-form walk as a silent fallback, but the
   fallback output is a legal pre-task-9 spelling and the risk is
   bounded to a pairing hint; accepted.
6. **File audit** passes; the two `bad-division` header warnings
   (`parser.h`, `sem_binder.h`) predate pa22 — the pa22 diff adds zero
   function bodies to headers (checked `git diff 2eb173d77 HEAD` for
   `{`-lines in `sem_binder.h`), and `lower_name_parts.h` holds only
   declarations and two small inline table helpers.

### Defects found and fixed

7. **SFINAE probes swallowed class-body instantiation faults
   (wrong-code).** `DeduceFunctionTemplate`'s
   `catch (const std::exception&)` treated *any* fault during signature
   composition as a deduction failure — including faults from
   instantiating a class *body* under a completeness demand, which
   14.8.2p8 places outside the immediate context (hard error). Probe:
   `template<class T> struct Bad { typename T::missing m; typedef int
   type; }; template<class T> typename Bad<T>::type f(T); int f(...);`
   — our compiler silently selected `f(...)` (wrong output that looks
   right; the reference selects the template via lazy member semantics,
   the standard hard-errors; we matched neither). The plan's own Design
   section promised a hard-error boundary that was never implemented.
8. **Eager instantiation of dormant specializations at the
   definition-upgrade point (no-eager-instantiation violation).** Two
   pre-pa22 loops (`sem_spec.cpp`, `sem_member_template.cpp`)
   instantiated every not-yet-instantiated specialization record the
   moment the template's definition appeared — contradicting task 6
   (14.7.1p1), surfacing faults at the naming site (probe: forward
   declaration + `typedef X<int>* p;` + bad definition body → we
   hard-errored where the reference and the standard succeed), and
   bypassing `InstantiateSpecializationBody`'s partial-specialization
   match for records named before the definition.
9. **Conversion-template deduction recomputed its pattern per
   classification.** `DeduceOneConversionTemplate` re-resolved the
   conversion-type-id and built a fresh (model-lifetime) pattern-param
   scope on every class-source conversion classification;
   `tmpl.conversion_pattern` was written but never read, and the
   captured `tmpl.conversion_type` fact was re-derived from the AST.
10. **Out-of-class ctor/conversion-template definition pairing matched
    by arity only, last-wins.** Two same-arity constructor templates
    (or conversion templates, whose composed signatures are all
    `void() cv`) could mis-pair a definition with the wrong
    declaration.
11. **Decl-order visibility off-by-one for merged captures.**
    `capture_seq` is the stamp the *next* binding receives; a template
    redeclaration merging into an existing overload binding consumes no
    stamp, so the first namespace variable declared after it received
    `seq == capture_seq` and stayed visible to dependent lookup when it
    should be hidden (`>` vs `>=`).
12. **`FlattenExpr` fell off the end for `sizeof...(pack)` (undefined
    behavior).** `ast_text.cpp`'s value-returning switch had no
    `EK_SIZEOF_PACK` case (a `-Wswitch` warning on every build) — and
    the flattened spelling feeds the 14.5.6.1 template-identity
    comparisons pa22 relies on for enable_if-style defaulted non-type
    parameters. The AST debug printer had the same gap.
13. **Duplication and hygiene.** `FlattenDeduced`/`ArgBound` existed as
    two per-file anonymous-namespace copies (silent-drift risk); the
    `ImplicitConversion` and `DeclBinder` constructors initialized
    members out of declaration order (`-Wreorder` on every build); dead
    file-local helpers in `sem_expr.cpp`, `template_body.cpp`,
    `lower_convert.cpp`, `lower_new.cpp`, an unused variable in
    `sem_convert.cpp`, a missing `SB_VARIABLE_TEMPLATE` case in the
    scope dump's keyword switch, and three over-indented
    `shadow.capture_seq` insertions. The build is now warning-free.

### Accepted as-is (documented rationale)

- `Substitutions::Find` linear scan: bounded by per-symbol component
  counts, one mangling per emitted symbol.
- `SubstituteOrderingTypes` re-runs per ordering direction: partial
  ordering is tournament-shaped (O(n) comparisons per overload set),
  patterns are memoized via `pattern_ready`, so the rebuild is bounded.
- O(k²) signature comparisons across same-name template captures:
  bounded by per-name overload counts, a pre-existing shape.
- `0x7fffff00 + slot` synthesized array bounds in ordering: a
  documented improbable-collision device, contained in
  `template_order.cpp`.
- `CheckDependentPatternSlots` skips deferred-slot re-verification for
  pack-absorbed argument runs: an honest, commented subset boundary
  (structural deduction still validated the match); per-element pack
  re-substitution machinery is not warranted speculatively.
- `"L_DEPE"` dependent-value pairing hint and `(Scope*, name)` friend
  identity pairs: pre-existing PA21 conventions, presentation-only /
  small-list respectively.

## Changes Made

- `dev/src/sema/template_info.h`: new `InstantiationBodyFault`
  exception type (14.8.2p8 hard-error carrier); shared declarations for
  `ArgBound`/`FlattenDeduced`.
- `dev/src/sema/sem_template.cpp`: `InstantiateSpecializationBody` wraps
  body instantiation and rethrows escaping faults as
  `InstantiationBodyFault` (recursive-demand throws stay soft;
  `NotImplementedException` passes through for the exit-86 contract).
- `dev/src/sema/template_deduce.cpp`, `sem_spec.cpp`,
  `template_body.cpp`, `template_order.cpp`: the eight SFINAE/probe
  catch sites rethrow `InstantiationBodyFault` before swallowing.
- `dev/src/sema/sem_spec.cpp`, `sem_member_template.cpp`: removed the
  two eager definition-upgrade instantiation loops; dormant records now
  instantiate only at completeness demand.
- `dev/src/sema/template_order.cpp`: `DeduceOneConversionTemplate`
  composes the conversion pattern once per template
  (`conversion_pattern` cache now read; `conversion_type` fact reused;
  no per-call pattern scope).
- `dev/src/sema/sem_member_template.cpp`: out-of-class special-member
  template pairing now prefers a signature match (constructor
  signatures via `SameFunctionTemplateSignature`, conversion templates
  via positionalized conversion-type-id spelling), keeping the tolerant
  match only for a lone same-arity candidate.
- `dev/src/sema/scope_lookup.cpp`: dependent-lookup visibility gate
  uses `seq >= seq_limit`.
- `dev/src/sema/template_info.cpp`: shared `ArgBound`/`FlattenDeduced`
  definitions (both per-file copies removed).
- `dev/src/ast/ast_text.cpp`, `ast_printer.cpp`: `EK_SIZEOF_PACK`
  cases (flattened spelling `sizeof...(name)`; printer line).
- `dev/src/sema/sem_convert.h`, `decl_binder.cpp`: constructor
  initializer order matches declaration order.
- Dead-code removal: unused file-local helpers in `sem_expr.cpp`,
  `template_body.cpp`, `lower_convert.cpp`, `lower_new.cpp`; unused
  variable in `sem_convert.cpp`; `SB_VARIABLE_TEMPLATE` case in
  `scope.cpp`'s dump keyword switch.
- `cppgm.tests/course/pa22/100-dormant-forward-named-specializations.t`
  (+ ref fixtures generated via `make -C pa22 ref-test`): regression
  test for finding 8, verified byte-compatible with the reference
  before check-in.
- `pa22/plan.md`: Architecture Review and Final Architecture Review
  sections.

## Validation

- Probes 1–3 (reference-binary observation) validate the intrinsic
  model; probe 4 validates the conditional fold's side-effect behavior
  against the reference; probes 6–8 validate the two semantic fixes
  (probe 6 now exits 1 per the standard; probes 7–8 now exit 0 and
  match the reference's LowIR).
- `perl scripts/cppgm_file_audit.pl --stage pa22 --paths dev/src`:
  pass (two pre-existing header warnings, no pa22 contribution).
- `make -C pa22 test`: 176/176 fixtures + 1/1 course test.
- Root `make test-report-through-pa22`: 1908/1908 after all audit
  changes (~10s wall for the full 22-stage suite) — no regression in
  pa1 through pa21.
- `make -C dev cppgm++` from clean: zero warnings.
