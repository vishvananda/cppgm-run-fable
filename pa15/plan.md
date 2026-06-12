# PA15 Plan: Object-Model LowIR Lowering

## Goal

Extend the PA14 procedural `cppgm++ --emit-lowir` path with the basic
non-polymorphic object model: class layout, member lookup and access control,
`this` / `.` / `->`, non-virtual methods, constructors/destructors with
lifetime, single inheritance, ordinary operator overloading + ADL, bit-fields,
aggregates, and namespace-scope object lifetime through `@__cppgm_init` /
`@__cppgm_fini`.

## Ownership boundaries

The existing pipeline stays:

- `DeclBinder` (PA11, dev/src/sema/decl_binder.*) owns scope formation,
  declaration binding, and class completion.
- `SemBinder` (PA12, dev/src/sema/sem_binder.*) + `SemExprAnalyzer`
  (sem_expr.*) own statement/expression analysis and build the `SemUnit`
  tree of `SemNode`s.
- `LowerProgram` (lower_unit.cpp) + `FunctionLowerer` (lower_function.cpp /
  lower_expr.cpp) own LowIR emission from the resolved `SemUnit`.

PA15 additions respect those boundaries:

1. **Class metadata (sema-owned).** A new `sema/class_info.h/.cpp` module owns
   one `ClassInfo` record per completed class entity, registered in a
   `ClassRegistry` owned by `TypesModel`. It holds:
   - fields in declaration order: name, type, byte offset, bit-field facts
     (storage unit type, bit offset, bit width), mutable flag, access,
     default-member-initializer AST pointer, anonymous-member injection
   - the single direct base (entity + access)
   - per-member access for the class scope's bindings (stored on
     `ScopeBinding`: one access per overload for functions)
   - constructor overload set (signature, access, explicit/deleted/defaulted,
     mem-initializer + body AST), destructor, aggregate-ness
   - friends (function entities and classes) for access checking
   Layout (offsets, size, alignment, bit-field packing, `alignas`) is computed
   by one shared layout routine in this module, used for ordinary fields,
   bit-fields, anonymous members, and the base subobject (offset 0).

2. **Semantic analysis (SemBinder/SemExprAnalyzer).** In-class member function
   bodies, ctor-initializers, and default member initializers are *deferred*
   until the outermost enclosing class completes, then analyzed with the class
   scope + `this` context. Member access, method calls, operator overloading,
   ADL, and access control are resolved here; resolved facts (member byte
   offset, base-subobject hops, bit-field facts, callee entity, ctor/dtor
   identity) are stored as typed `SemNode` fields. The lowering never re-does
   lookup or layout.

3. **Lowering (LowerProgram/FunctionLowerer).** Consumes the resolved facts:
   - class-typed slots `obj<SxA>`, member addressing through
     `index i8 [projection=base_subobject|field]`
   - method calls with the explicit `%this` first argument
   - demand-driven emission of ctor/dtor helpers (C1 `Class__Class`,
     C2 `Class__Class__base_entry`, D1 `Class___Class`, D2 base entry),
     `alias object` lines for the identical entry
   - destructor cleanups at block exit / return / break / continue, and the
     `eh_try`/`eh_end`/`resume` unwind-dispatch regions around may-throw calls
     made while cleanups are live (plus the two `__external_runtime` declares)
   - `@__cppgm_init` / `@__cppgm_fini` from registered dynamic
     init/destruction actions of namespace-scope objects
   - Itanium mangling extensions (nested names, const member functions,
     C1/C2/D1/D2, operator codes) in lower_name.cpp

## SemNode contract additions

New/extended facts on `SemNode` (lowering input):

- member expressions: `member_offset`, `base_hops` (count of
  base_subobject projections), bit-field facts, `is_static_member`
- method calls: callee entity = (class member scope, name, this-adjusted
  type); the object argument is an explicit address-typed child so call
  lowering stays uniform
- `SN_CONSTRUCTOR_ACTION` generalized: ctor entity + variant + argument
  expressions; new `SN_DESTRUCTOR_ACTION` facts attached to locals/temps so
  the lowering tracks cleanups
- function definitions: `is_method`, ctor/dtor variant tag so
  emission/mangling/binding (weak for in-class, strong for out-of-class)
  follow from typed state

## Naming and comparison conventions (pinned by refs)

- methods `@Class__name`, ctors `@Class__Class` (+`__base_entry` for C2-only
  demand), dtors `@Class___Class`, operators sanitize symbols to `_` per
  char with spaces dropped (`operator==` -> `operator__`,
  `operator new` -> `operatornew`), overload collisions get `__ov<N>`
- in-class definitions: `binding=weak`, demand-emitted; out-of-class:
  `binding=strong`, source-owned
- synthesized ctors/dtors get `unwind=no` when their actions cannot throw
- the relaxed comparator canonicalizes top-level order and most metadata but
  requires exact instruction text (temp numbering, slot names, labels)

## Validation plan

- iterate with `make test-report ACTIVE_TEST_REPORT_PAS='pa15'`, diffing
  `.my` output against `.ref` per test
- gate every milestone with `make test-report-through-pa15` (PA11/PA12/PA14
  dumps must stay byte-identical; class-related behavior previously rejected
  must not regress earlier stages)
- `perl scripts/cppgm_file_audit.pl --stage pa15 --paths dev/src` for layout
  and file-size discipline; split new logic into cohesive modules
  (class_info, member analysis, ctor/dtor synthesis, lifetime lowering)

## Implementation order

1. ClassInfo registry + full layout (fields, base, bit-fields, alignas,
   anonymous members); access recording; base-aware member lookup;
   injected-class-name.
2. Methods: deferred in-class bodies, out-of-class definitions, `this`,
   member access (`.`/`->`/implicit), static members, method calls;
   lowering for obj slots, member addressing, method emission + mangling.
3. Ctors/dtors: user + synthesized, mem-initializers, default member inits,
   aggregate/value/direct init, locals + temporaries + globals, lifetime
   cleanups, eh_try machinery, `__cppgm_init`/`__cppgm_fini`.
4. Operator overloading + ADL + hidden friends; friend access.
5. Long tail: bit-field access, pseudo-destructors, inheriting ctors,
   placement new, user-defined literal operators, static thread_local
   members, using-declarations re-exposing members.

## Resolved conventions (from reference diffs)

- User-defined string literals: `operator ""_x` parses from the combined
  `""_x` token (PTK_UD_STRING + ud_suffix); the call passes the literal's
  global address plus `elements - 1` as an int converted to the size
  parameter; mangling is `li<SourceName>`.
- `T{...}` braced functional casts parse as the call shape with the braced
  items as arguments (an id followed by `{` is never a value use).
- Identity casts (same source and target type) emit no conversion copy.
- Synthesized lifetime bodies elide subobject ctor/dtor calls whose whole
  chain does nothing (`DefaultConstructionHasEffects` /
  `DestructionHasEffects`); the elided constructor chain still demands the
  nearest user-provided constructors (emitted on use, `elided` actions),
  while the destructor side demands nothing. Expression-level construction
  and destruction always call.
- Full-expression temporaries of destructible classes register cleanups as
  they construct; every may-unwind construction in such a statement runs
  under its own dispatch region (registration closes and reopens the open
  region), the temporaries destroy in reverse order inside the last region,
  and dispatch blocks destroy live temporaries before scoped locals. The
  unwind-runtime declares appear only for scoped local cleanups.

## Architecture Review

As built, the implementation matches the planned ownership split:

- **Layout** lives only in `sema/class_info.cpp`: `BeginClassLayout` /
  `LayoutField` / `LayoutBitField` / `FinishClassLayout` compute offsets,
  bit-field packing, size, and alignment once at class completion. No
  offset/alignment arithmetic exists in lowering; `lower_member.cpp` addresses
  members purely from the typed `SemNode` facts (`member_offset`, `base_hops`,
  `member_ref`, bit-field unit/offset/width).
- **Resolution** lives in the `sem_*` modules. Member access, method calls,
  ctor/dtor selection, operator overloading, and ADL stamp
  `entity_scope`/`entity_name`/`type`/`special` plus offset facts onto
  `SemNode`s (`sem_member.cpp`, `sem_ctor.cpp`, `sem_operator.cpp`). Lowering
  performs no AST walks (no `Ast*` references in `lowering/`) and no name
  lookup; demand-driven member emission keys off `MemberDefinitionKey`
  (scope + name + signature + kind), not source spelling.
- **Demand-driven helpers**: deferred in-class definitions register weak in
  `member_defs_`; `MemberFunctionEntry` attaches the definition when an entry
  is first demanded, and `LowerUsedFunctions` sweeps entries in registration
  order so output stays repeatable. C1/D1 entries carry the base-entry alias;
  unused weak definitions never print.
- Two deliberate, reviewed exceptions to "lowering only consumes facts":
  - `LowerOverloadIndex`/`LowerMemberOverloadIndex`/`LowerOverloadDeleted`
    read the declaring scope's own binding (`FindOwnBinding`, no scope-chain
    walk) to derive the `__ov<N>` presentation suffix and per-overload
    deleted-ness. Both are entity-keyed reads of sema-owned per-overload data
    used only for symbol naming/declare emission; call dispatch itself is
    driven by `SemNode` facts.
  - `MangleTerminalName` string-parses `operator...` spellings when building
    Itanium names. The spelling is itself the semantic identity of an
    operator, and the output is presentation-only.
- **Principled recovery paths** (verified not to be acceptance shortcuts):
  the builtin-call fallback in `AnalyzeCall` only fires for the three
  `__builtin_*` names and rethrows the original lookup error otherwise;
  `TryVexingCallRecovery` is the 6.8p1 statement/declaration disambiguation,
  keyed on whether the leading name resolves to a type or a function, with a
  structural guard and fall-through to ordinary declaration binding.

## Final Architecture Review

Post-audit state (see `audit.md` for the change list):

- The `LowerUsedFunctions` fixpoint no longer re-walks the whole function
  table per round: `DemandFunction` is the single demand chokepoint and
  records the lowest re-armed entry index, and each sweep restarts from that
  floor. Lowering order — and therefore symbol uniquification and output —
  is unchanged; the through-pa15 report is byte-stable.
- Constructor/field elision (`DefaultConstructionHasEffects` /
  `DestructionHasEffects`) recurses over the subobject tree once per
  class analysis; remaining linear scans (`FindClassField`, ctor overload
  walks, scope-depth string building in `LowerScopeKey`) are bounded by
  per-class member counts and nesting depth, not program size.
- File discipline: every PA15 module is within the audit's size and
  header-body limits; the only file-audit warning (`parser.h`) predates PA15
  (last touched in the PA6 audit). No implementation lives outside
  `dev/src`; the only handout-path change in the PA15 range is this plan.
- Extension points are clean for the next stages: PA16 value semantics can
  add copy/move ctor entries to `ClassCtor`/`member_defs_` and new
  `SN_CONSTRUCTOR_ACTION` variants without touching layout or demand
  plumbing; PA17 vtables slot into `ClassInfo` layout plus a new dispatch
  path in `lower_member.cpp`.
