# PA26 Audit

## Audit Plan

Scope: commits `2c5590244..dfe8fd714` (PA26 slice: non-virtual multiple
inheritance object model, member pointer semantics + lowering,
dynamic_cast<void*> offset-to-top lowering, ADL CRTP cycle guard, member
template member definitions), audited against `pa26/README.md`,
`pa26/plan.md`, and the pa14–pa25 regression surface.

### Files to inspect

- `dev/src/sema/class_info.{h,cpp}` (+164/+50): the new ordered direct-base
  table, base offsets, and derivation-path/ambiguity queries. Verify paths and
  ambiguity are computed from the typed base table (not strings), verify the
  path query does not do exponential DAG walks, and verify empty-base layout
  matches the refs as a general rule, not fixture-shaped constants.
- `dev/src/sema/sem_bases.cpp` (new, 127): base-clause binding, pack-expanded
  base specifiers. Verify polymorphic-extra-base rejection stays a typed
  boundary (`OutsideBoundary`) and non-first-polymorphic layout is real.
- `dev/src/sema/scope_lookup.{h,cpp}`, `sema/scope.h`: sibling base links and
  DAG member lookup with 10.2 ambiguity. Check for repeated scans, visited-set
  handling, and that ambiguity is an ordinary substitution failure in SFINAE
  context (not a test-specific gate).
- `dev/src/sema/sem_member_pointer.cpp` (new, 246) +
  `dev/src/lowering/lower_member_pointer.cpp` (new, 123): pm formation,
  `.*`/`->*`, null comparisons, base-to-derived conversion, nontype pm
  template args/deduction/SFINAE; i64/i128 value forms. Verify pm constants
  are typed const-eval values (entity + offset), not strings; verify call
  lowering is a real indirect call, not a trampoline or fixed dispatch table.
- `dev/src/sema/sem_ctor.cpp` (+178), `sem_special.cpp` (+106): generated
  special members looping the base table, mem-initializer matching,
  pack-expanded mem-initializers. Check destruction order (reverse), copyobj
  classification stays whole-object, and no per-base quadratic re-lookup.
- `dev/src/sema/sem_expr.cpp` (+236), `sem_operator.cpp`, `sem_convert.cpp`,
  `sem_apply.cpp`, `sem_cast.cpp`: derivation-path plumbing replacing
  `base_hops`; dynamic_cast<void*> acceptance. Verify acceptance is
  type-system-driven (polymorphic class check), not shape-driven.
- `dev/src/lowering/lower_member.cpp`, `lower_expr.cpp` (+103),
  `lower_convert.cpp`, `lower_eh.cpp` (+46): per-edge base-subobject
  projections vs. the single-total-offset presentation the fixtures pin;
  dynamic_cast<void*> shape incl. the dead runtime block. Verify the shape is
  general (offset-to-top scan) and reuses `RttiRef`/external runtime refs.
- `dev/src/lowering/lower_name.cpp` (+46): `M<class><member-type>` manglings
  and `Xad L_Z…E E` nontype pm args. Verify built from structured name parts.
- `dev/src/sema/sem_template*.cpp`, `template_args.cpp`, `template_deduce.cpp`,
  `sem_pack.cpp`: pm nontype args, deduction, pack-expanded bases/mem-inits,
  member-template member definitions, partial-explicit overload sets (14.8.1).
- `dev/src/sema/sem_node.h`, `sem_binder.h`, `lower_function.h`: header growth
  vs. the bad-division warnings; verify no implementation moved to headers to
  dodge the file-size audit.

### Performance risks to check

- Derivation-path search over the base DAG: memoization or bounded walk; no
  exponential re-walks for deep/diamond-free hierarchies; path queries not
  recomputed per expression when cacheable on the node.
- Member lookup across sibling bases: visited set, no repeated whole-scope
  scans per name; ambiguity detection not O(paths²).
- Special-member synthesis: single pass over the base table, no per-base
  re-resolution of the same ctor overload sets.
- Mangling: no repeated substring scans over growing strings for `M` types.

### Ownership boundaries to check

- `sema/class_info.*` owns base tables/offsets/paths; lowering only renders
  typed `base_steps`/pm facts — grep for lowering re-deriving offsets, hop
  counts, or member identity from names/strings.
- pm constants: typed (member entity + adjustment), no string-encoded members.
- No fixture-name, source-shape, or test-count gates anywhere in dev/src.
- `paN/` handout/ref/script files untouched by the slice.

### File-audit issues to check

- `perl scripts/cppgm_file_audit.pl --stage pa26 --paths dev/src` must pass;
  4 pre-existing bad-division warnings (lower_function.h, parser.h,
  sem_binder.h, sem_expr.h) — verify PA26 did not grow them with new
  implementation bodies to bypass the .cpp size gates.
- New files (`sem_bases.cpp`, `sem_member_pointer.cpp`,
  `lower_member_pointer.cpp`) must be self-contained modules, not
  implementation fragments split to dodge size limits.

## Findings

Every finding below was reproduced with a probe program before fixing and
re-probed after; suites stayed green throughout (pa26 51/51, through-pa26
2481/2481 after each slice).

### Fixed - correctness (silent wrong code or wrong acceptance)

1. **12.8 predicates walked only the first base** (`sem_special.cpp`):
   `SubobjectCopyUnavailable`, `SubobjectMoveUnavailable`,
   `AssignBlockedByMembers`, `SubobjectsConstCopy` tested `cls.base` only. A
   deleted-copy second base did not delete the implicit copy ctor; a
   `B(B&)`-only second base did not force the `D(D&)` form (const-correctness
   miscompile).
2. **Inheriting constructors ignored extra bases** (`sem_class.cpp`):
   `using A::A;` in `D : A, B` left the B subobject unconstructed (12.9p8),
   and `using B::B;` naming a non-first base was rejected outright.
3. **Using-imported field from a displaced base mis-addressed**
   (`sem_member.cpp`): the "home-first" path resolved to offset 0 while
   `member_offset` stayed owner-relative - `d.y` read `A::x`'s storage. The
   fix addresses through the owner's subobject and keeps the pinned
   projection-free import presentation only while that subobject sits at
   offset 0.
4. **Null pointers did not survive displaced-base conversions**
   (`lower_convert.cpp`): a runtime-null `D*` converted to a second-base `B*`
   became `(B*)4`. Data member pointers had the same defect (`null + 4`), and
   function member pointer conversions dropped the offset entirely. All three
   now branch so null stays null; offset-0 conversions keep the exact
   unguarded PA15-25 shapes.
5. **Base-to-derived static_cast lowered as a copy** (`sem_cast.cpp`,
   `lower_member.cpp`, `lower_expr.cpp`): pointer and reference downcasts
   from a displaced base view kept the base address. Reference downcasts now
   stamp a typed reverse adjustment (`SemNode::base_reverse`); pointer
   downcasts shift by the negated path offset behind a null guard.
6. **Member pointer calls lost the this-adjustment** (`lower_convert.cpp`,
   `lower_member_pointer.cpp`, `lower_expr.cpp`): calling through a
   `void (D::*)(int)` holding a displaced base's method passed the
   unadjusted object. Conversions now fold the offset into the i128's high
   half and call sites of displaced-base classes apply it; classes without
   displaced bases keep the pinned adjustment-free call shape.
7. **Protected members through a non-first base were rejected; base-specifier
   access was never enforced** (`sem_class.cpp`): the 11.2/11.4 walks
   followed only the primary chain, and `ClassDirectBase::access` was
   write-only (PA26 had silently started accepting access-specified extra
   bases that PA21-25 rejected - a weakened check). Both walks now cover the
   base DAG, and each non-public base edge on the naming class's derivation
   path restricts member naming (`d.y` through `struct D : private B` is now
   an error outside D).
8. **Sibling using-imports were wrongly ambiguous** (`scope_lookup.cpp`):
   two bases re-exposing one declaration through using-declarations threw an
   ambiguity error; 10.2p3 makes them the same entity (identity = shared
   original owner scope).
9. **Member pointer non-type arguments over-accepted and mis-valued**
   (`sem_member_pointer.cpp`, `lower_member_pointer.cpp`,
   `lower_name.cpp`): a base member's pm converted to a derived-class
   parameter (14.3.2p5 applies no conversions; g++ rejects, so SFINAE
   diverged), the folded data value missed the displacement, the re-formed
   constant call missed the object adjustment, and plain member-function pm
   args mangled without cv/signature (`XadL_ZN1B3setEE` vs g++'s
   `XadL_ZN1B3setEiEE`). Arguments are now class-exact; the constant call
   path still steps down to the owner subobject as defense.
10. **Silent lowering fallbacks** (`lower_member.cpp`, `sem_apply.cpp`): a
    non-unique derivation path returned the *unadjusted* address (wrong
    codegen on desync) and an ambiguous inherited-conversion subobject
    silently skipped its adjustment. Both now throw.

### Fixed - performance

11. **Exponential base-DAG walks** (`class_info.cpp`, `scope_lookup.cpp`):
    `BaseSubobjectPath` enumerated every derivation path (2^n on diamond
    ladders; measured ~2s at 24 levels) and sits in overload-ranking hot
    paths; member lookup and the `DerivedFrom` walks revisited shared
    subtrees. Path counts are now memoized per query (capped at 2 - the only
    distinctions 10.2 needs) and the walks carry visited sets; a 26-level
    diamond ladder compiles in ~12ms. `BaseAccessPath` walks the unique path
    guided by the same memo instead of enumerating.

### Inspected - justified as-is

- **`fn_pointer_fold`** (`scope.h`, `sem_lifetime.cpp`, `sem_member.cpp`):
  reads of an instantiated const function-pointer static member fold to a
  declare-only function named by the member. The checked-in oracle pins
  exactly this presentation (`300-dependent-member-template-nontype-target-
  overload.ref` emits `declare function @value ... [binding=internal]` and
  `addr @value` while `__cppgm_init` stores the real target); the gate is
  typed (instantiated + const + fn-pointer + non-constant init), not
  fixture-named, and the storage keeps its true dynamic initialization.
- **Empty bases share offset 0 regardless of type** (`class_info.cpp`
  layout): stricter Itanium would displace same-type empty subobjects, but
  the oracle pins the shared layout - `100-public-qualified-base-typedef-
  ambiguous-subobject.ref` requires `obj<1x1>` for a class holding two
  `impl` subobjects and chains two offset-0 projections.
- **`ClassInfo::base` duplicating `direct_bases[0]`**: a documented
  primary-chain alias with a single writer (`sem_bases.cpp`); kept because
  the whole PA15-25 surface reads it. The 12.8 predicates were the last
  readers that should have used the table and now do.
- **`CollectClassAndBases` allocation in conversion classification**: a
  per-call vector of class pointers with O(k^2) dedup where k = classes in
  the DAG (single digits in practice); no measurable suite slowdown.
- **Lowering deriving paths from types at conversion sites**: conversions
  carry types, not stamped nodes; the path fact has a single owner
  (`BaseSubobjectPath` in `class_info.cpp`) and desyncs now throw instead of
  emitting unadjusted addresses (finding 10).
- **`static_cast` between unrelated class pointer types is accepted**
  (pre-PA26 lenience): invalid inputs are undefined for the assignment;
  noted, unchanged by PA26.
- **Pointer logical operands compare in `ptr` space, member pointers in
  their integer value space** (`lower_expr.cpp`): a type-driven presentation
  rule, not a fixture gate.

## Changes Made

- `dev/src/sema/sem_special.cpp` - 12.8 predicates iterate `direct_bases`.
- `dev/src/sema/sem_class.cpp` - inheriting ctors accept any direct base and
  default-initialize the others in declaration order; `CheckMemberAccess`
  enforces base-edge access (new `CheckBaseEdgeAccess`) and walks the DAG
  for protected members and 11.4 friend grants.
- `dev/src/sema/sem_member.cpp` - imported-member addressing through the
  owner subobject.
- `dev/src/sema/sem_cast.cpp` - reference downcasts stamp the typed reverse
  adjustment (`base_reverse`).
- `dev/src/sema/sem_apply.cpp` - inherited-conversion ambiguity throws.
- `dev/src/sema/sem_member_pointer.cpp` - pm non-type args are class-exact
  (14.3.2p5).
- `dev/src/sema/scope_lookup.cpp` - visited-set member lookup; import
  copies dedup by original owner (10.2p3).
- `dev/src/sema/class_info.{h,cpp}` - memoized `BaseSubobjectPath`,
  visited-set `DerivedFrom*` walks, `ClassHasDisplacedBase`,
  `BaseAccessPath` edges, `ClassBaseEdge`.
- `dev/src/sema/sem_node.{h,cpp}` - `base_reverse` fact.
- `dev/src/sema/sem_binder.h` - `CheckBaseEdgeAccess` declaration; unsplice
  a tab-joined declaration pair; `MemberInitPlan` moved to its only
  consumer (`sem_ctor.cpp`), keeping the header inside the size gate the
  audit additions had pushed past.
- `dev/src/lowering/lower_convert.cpp` - null-guarded displaced-base
  pointer/data-pm/function-pm conversions (`AdjustPointerGuarded`).
- `dev/src/lowering/lower_member.cpp` - `AdjustToBase` throws on non-unique
  paths; `MemberAddress` renders reverse adjustments.
- `dev/src/lowering/lower_member_pointer.cpp` - constant pm calls step down
  to the owner subobject; runtime pm calls of displaced-base classes apply
  the value's this-adjustment.
- `dev/src/lowering/lower_expr.cpp` - pm call assembles the (possibly
  adjusted) object after the callee, preserving the pinned evaluation
  order.
- `dev/src/lowering/lower_name.cpp` - member-function pm args mangle with
  cv-qualifiers and bare signature.
- `dev/src/lowering/lower_function.h` - declarations for the above.

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa26 --paths dev/src` - pass
  (4 pre-existing bad-division warnings, none grown by PA26 or this audit).
- `make test-report ACTIVE_TEST_REPORT_PAS='pa26'` - 51/51 after every
  slice.
- `make test-report-through-pa26` - 2481/2481 (pa1-pa26, all stages) after
  every slice; no earlier-assignment regressions.
- Probe programs (displaced-base copy/assign deletion, inherited ctor over
  MI, imported-field addressing, null up/downcasts, pm value and call
  adjustment, pm non-type args, base access, 26-level diamond ladder)
  compiled and inspected before/after each fix; g++ cross-checked the
  member-pointer argument and mangling behavior.
