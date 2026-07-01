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

### LowIR ABI (lowering/lower_value.cpp + lower_function/expr/member)

Boundary classification, derived from typed ClassInfo facts only:
- direct form (`obj<SxA>` parameter / return) iff the class is
  trivially copyable, not a union, and sizeof ≤ 16;
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
  cleanup machinery; materialized argument objects register their
  cleanups exactly like other temporaries.

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
  `lowering/lower_value.cpp` (class ABI + value transfer lowering),
  both registered in `dev/frontend_source_sets.mk`.

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
