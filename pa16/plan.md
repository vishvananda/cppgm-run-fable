# PA16 Plan: `cppgm++ --emit-lowir` value semantics

PA16 extends the PA15 non-virtual object model with usable value
semantics: copy/move special members, by-value class passing/returning,
temporary materialization, delegating and out-of-class special members,
new/delete, unions, conversion operators, and ADL for ordinary calls.
Everything is an extension of the existing pipeline (PA10 AST → PA11/12
SemBinder/SemExprAnalyzer → PA14/15 LowIR lowering); no second model.

## Compiler design

### Class metadata (sema/class_info.*)

- `ClassCtor` gains `kind` (ordinary / copy / move), `implicit`
  (implicitly declared, not user-written), and deleted-participation
  facts. User copy/move constructors are classified at declaration by
  parameter shape (12.8p2/p3).
- At `CompleteClass`, the binder implicitly declares (12.8):
  - a copy constructor unless the user declared one; defined as deleted
    when the class has a user move ctor/assignment or an unmovable
    subobject;
  - a move constructor iff no user copy ctor, copy assignment, move
    assignment, or destructor; when defined as deleted it is *ignored*
    by overload resolution (12.8p9);
  - copy/move `operator=` under the analogous rules, declared into the
    class member scope binding so the existing operator machinery
    resolves them. Implicit entries are flagged so selection triggers
    demand synthesis and so `has_user_ctor` / aggregate-ness / PA15
    default-init logic is not perturbed (monotonic extension).
- Triviality queries (free functions over ClassInfo links):
  trivially-copyable, trivial copy/move ctor, trivial dtor. They drive
  both the LowIR ABI and the `copyobj` shortcut forms.

### Special-member synthesis (sema/sem_special.cpp, new file)

Demand-driven like `EnsureImplicitDefaultCtor`: when overload
resolution selects an implicit copy/move ctor or assignment, the binder
synthesizes the deferred SN_FUNCTION_DEFINITION (weak, demand-emitted):
- base-wise then field-wise copy/move actions; a leading trivially
  copyable storage prefix lowers as one `copyobj` span (a dedicated
  storage-copy SemNode); class subobjects with non-trivial copy call
  their own copy/move members; scalar/bit-field tails reuse the
  member-assign machinery.
- assignment operators return `*this` (lvalue ref → `return ptr`).
- unions copy their whole storage with one `copyobj`; trivial union
  subobject destructor steps are omitted from enclosing synthesized
  destructors.

### Value transfers in the semantic tree

A single wrapper convention: copy-initializing a class object (declared
locals, by-value call arguments, by-value returns) resolves the
copy/move constructor semantically (access, deleted, explicit checks)
and wraps the source in an SN_CONSTRUCTOR_ACTION marked `synth_copy`.
If the selected ctor is the implicit trivial one, the action is also
marked as a trivial transfer so the lowering emits raw `copyobj`
without demanding a helper definition (the accepted PA16 direct
value-transfer form). The PA12 dump printer skips `synth_copy`
wrappers, so PA12/PA13 dump outputs are unchanged (monotonic
extension). Sources that are already same-class prvalue constructor
actions keep the existing 12.8p31 elision path (construct in place).

Move selection: rvalue sources (xvalues/prvalues) bind `C&&` candidates
first via the existing reference-binding ranking; `return local;` tries
the move ctor first per 12.8p32, falling back to copy.

### LowIR ABI (lowering/lower_types + lower_function/expr/member)

Boundary classification, derived from typed ClassInfo facts only (the
classifier landed in `lower_types.cpp` — `LowerAbiParameter` /
`LowerAbiReturn` over `class_info`'s `ClassParamDirect` /
`ClassReturnDirect` — rather than the separate `lower_value.cpp` this
plan originally named; value-transfer lowering lives in
`lower_expr/lower_member/lower_convert`):
- direct parameter form (`obj<SxA>`) iff the class has a trivial move
  constructor and trivial destructor, is not a union, and sizeof ≤ 16;
  direct returns additionally require a trivial copy constructor;
- otherwise indirect: parameters `ptr [pass=by_address]`, returns
  through a leading `%ret : ptr [pass=indirect_result]` with `-> void`.

Call sites and definitions agree by construction since both consult the
same classifier on the function type.

- Definitions: direct-obj params copy into their named slot on entry
  (`copyobj SxA %p, addr`); by_address params keep their `obj` slot
  declared but address through `%p` directly; indirect returns thread
  `%ret` through return lowering.
- Calls: direct-obj arguments materialize `$argobj__N` slots (raw
  `copyobj` for trivial transfers, in-place construction otherwise) and
  pass the slot; by_address arguments materialize `$arg__N` and pass
  the address temp; indirect results materialize the destination (the
  declared object when initializing one — elision — else `$arg__N` /
  `$tmpobj__N`) and pass its address first.
- Returns: direct form copies/constructs into `$retobj__N` and emits
  `return obj<SxA> $retobj__N`; indirect form constructs into `%ret`
  and returns void. An eligible top-level named local returned by every
  class-valued return statement is lowered directly in `%ret`
  (return-slot reuse); its scope cleanup is dropped with ownership.
- Destructor-protected temporaries reuse the PA15 full-expression
  cleanup machinery; materialized by-value argument objects are the
  exception — the callee destroys its parameters, so the caller
  registers no cleanup for them.

### Remaining features

- Delegating constructors: a mem-initializer naming the class itself
  becomes a single complete-object constructor call on `this`; no other
  initialization runs in the delegating body.
- Out-of-class constructor *and destructor* definitions (and
  out-of-class `= default`) extend `BindQualifiedSpecialMember`;
  ref-qualified member declarations parse into the function type
  (new ref-qualifier facts) and participate in implicit-object binding.
- new/delete: scalar and array forms lower to the builtin
  `operator new` / `operator new[]` / matching deletes (declared
  on demand like other builtins), with construction/destruction as
  ordinary actions over explicit storage; array new of classes with
  non-trivial destruction stores the element count in an 8-byte header;
  nothrow placement forms branch around construction on null.
- Conversion operators: parsed as members (`operator T()`), recorded in
  ClassInfo, and added to `ClassifyConversion` as a second user-defined
  conversion source (alongside converting constructors), including
  contextual bool and explicit-operator rules.
- ADL for unqualified function calls: associated namespaces and hidden
  friends merge into the candidate set (reusing the operator-ADL
  collector); a parenthesized callee suppresses ADL.

## Ownership boundaries

- sema owns all overload resolution, special-member declaration state,
  triviality facts, and the action tree; the lowering only reads
  resolved callees, layout facts, and the classifier over ClassInfo.
- lowering owns slot naming, ABI shapes, copyobj emission, and
  return-slot reuse; it never re-derives semantics from names.
- New files keep audit limits: `sema/sem_special.cpp` (synthesis),
  `sema/sem_new.cpp`, `sema/sem_cast.cpp`, `lowering/lower_new.cpp`,
  and `lowering/lower_convert.cpp`, all registered in
  `dev/frontend_source_sets.mk` (the class ABI lives in the existing
  `lowering/lower_types.cpp` instead of a separate `lower_value.cpp`).

## Validation

- Iterate per cluster with `make test-report ACTIVE_TEST_REPORT_PAS='pa16'`,
  diffing `*.my.lowir.compare.diff` artifacts against checked-in refs.
- After each cluster lands, run `make test-report-through-pa16`; any
  older-stage regression (notably PA12 dump shapes and PA15 LowIR) is a
  blocker fixed before continuing.
- `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src` must
  stay clean; commit per cohesive cluster.

## Status (2026-07-01)

Complete: `make test-report-through-pa16` passes 1152/1152 and the
pa16 file audit is clean. Late-landing design points beyond the
original plan:

- Body-derived non-throwing facts: every bound definition publishes
  `fn_unwind_no` (and ctor/dtor records) from `NodeMayThrow` over its
  body; unwind-dispatch regions and the EH runtime declares emit only
  when a call can actually unwind at a point with live cleanups.
- Callee-owned by-value parameters: sema attaches the destructor
  action to the parameter node; the lowering registers it as a
  function-scope cleanup.
- Value-transfer shapes pinned by the refs: non-trivially-copyable
  conditionals materialize their own temporary and copy-construct the
  destination; braced local class arrays share one base address with
  byte-offset element construction (namespace-scope arrays keep the
  subscripted global-init form); synthesized assignment copies
  bit-field storage units as scalar unit stores ahead of the trivial
  storage-prefix `copyobj`.
- Elided-copy odr-use (3.2p3/12.8p30): a synthesized ctor selected for
  an elided return copy emits the user-provided member specials its
  body odr-uses without emitting the synthesized definition itself.
- Per-overload owner tracking (`ScopeBinding::fn_owner`) keeps
  using-imported members addressed and emitted under their declaring
  class; built-in operator forms compete in operator overload ranking
  (13.3.1.2p3); conversion functions win reference-temporary binding
  over converting constructors (8.5.3p5).
- File layout after the audit split: `sema/sem_cast.cpp` (explicit
  conversions, sizeof/alignof) and `lowering/lower_convert.cpp` (value
  conversion emission) joined the source sets.

## Architecture Review

As built, the implementation matches the planned ownership split, with
one file-layout deviation reconciled above (no `lower_value.cpp`; the
ABI classifier joined `lower_types.cpp`, value transfers lower in
`lower_expr/lower_member/lower_convert`):

- **One ABI classifier.** `LowerAbiParameter`/`LowerAbiReturn`
  (`lower_types.cpp`) over `ClassParamDirect`/`ClassReturnDirect`
  (`class_info.cpp`) is the only direct/by_address/indirect_result
  decision point. Function definitions, `declare` rendering, direct
  call sites, indirect-call `as (...)` signatures, and result
  materialization all consult it, so call sites and definitions agree
  by construction.
- **Sema owns resolution and the action tree.** Copy/move selection,
  deleted/explicit/access checks, special-member declaration
  (`DeclareImplicitSpecialMembers` at CompleteClass) and demand
  synthesis (`sem_special.cpp`) all live in sema; value transfers are
  `SN_CONSTRUCTOR_ACTION` wrappers with typed flags (`synth_copy`,
  `trivial_copy`, `ctor_addressed`, `needs_dtor`), and — post-audit —
  call/conditional result temporaries pin their resolved destructor
  action in `SemNode::result_dtor` instead of lowering fabricating the
  callee.
- **Lowering owns slots, ABI shapes, copyobj, and return-slot reuse.**
  `ScanReturnSlotReuse` is a conservative every-return analysis;
  demand-driven emission keys off typed `(scope, name, signature,
  special)` entries; the callee-owned by-value parameter convention is
  implemented at the ABI boundary (callee destroys; the caller
  registers no cleanup for argument objects).
- **Triviality facts** are free functions over ClassInfo links,
  memoized post-audit, consumed by both the sema transfer logic
  (`TransferTrivial`, per-axis for ctor vs assign forms) and the ABI
  classifier; lowering reads only the SemNode flags sema sets.
- **Deliberate presentation-keyed exceptions** (PA15 precedent):
  Itanium mangling parses operator spellings; conversion-function
  member bindings are keyed by the canonical rendered
  `operator <type>` name; lowering-internal `pass=`/variant-code
  strings are produced and consumed inside lowering with total
  mappings.

## Final Architecture Review

Post-audit state (see `audit.md` for findings, probes, and the change
list):

- **Correctness vs the reference oracle.** The audit fixed wrong-code
  and accept-invalid divergences found by probing against
  `pa16/cppgm++-ref`: synthesized assignment now uses the assignment
  triviality axis for its storage prefix; call-result and explicit
  destructor paths run synthesis and access checks through the binder;
  by-value argument objects are destroyed exactly once (callee-owned);
  effect-free destructor elision is confined to named locals and the
  braced-assignment RHS (temporaries and `delete` destroy whenever the
  class needs destruction); ambiguous operator overloads and ambiguous
  conversion-function sequences are rejected; same-level own +
  using-directive-imported functions resolve as one overload set;
  ordinary bit-field stores emit real masks.
- **Termination and complexity.** User-conversion classification is
  structurally bounded (nested classifications run standard-only per
  13.3.3.1.2p1), so mutually convertible classes terminate. The nine
  recursive class facts memoize per ClassInfo under a facts version
  bumped by the only post-completion mutators (out-of-class
  special-member binding), making per-expression triviality/destruction
  queries O(1) steady-state. `DemandTreeCallees` walks each synthesized
  definition once. Unwind-dispatch arming is an evaluation-order scan
  that matches the reference contract (arm only when a call runs under
  a live caller-owned temporary).
- **Remaining accepted shapes** (reviewed, not defects): the
  demand-sweep rescan floor; per-call-site function-reference string
  keys (linear, pre-PA16 convention); compile-time unrolling of
  fixed-bound class-array construction (ref-pinned output shape);
  body-derived `unwind=no` facts and EH-runtime declares, both pinned
  our way by the checked-in refs where the older ref binary differs.
- **Extension points for PA17.** Virtual dispatch can add vptr fields
  in `class_info` layout, vtable emission beside the existing global
  rendering, and virtual-call lowering keyed by new typed SemNode facts
  without touching the ABI classifier, the demand machinery, or the
  cleanup model. File-size headroom is the one watch item: a few
  functions sit within ~5-10 lines of the audit's 120-line cap and
  `sem_expr.cpp`/`sem_class.cpp` within ~100 lines of the file cap, so
  PA17 work in those areas should split early.
