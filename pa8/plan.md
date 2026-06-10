# PA8 (nsinit) Plan

## Goal

`nsinit` runs translation phases 1-7 over each srcfile (the PA7 pipeline
and semantic parser, extended to pa8.gram), links the translation units
into one program per 3.5/3.2, and writes the PA8 Mock Program Image.
Unlike PA7, diagnosable ill-formed programs that match pa8.gram must
exit EXIT_FAILURE; inputs outside pa8.gram remain undefined behaviour.

## What PA8 adds over PA7

- decl-specifiers `static`, `thread_local`, `extern`, `constexpr`,
  `inline` are recorded and semantically checked (7.1.1, 7.1.2, 7.1.5).
- expressions (literals incl. `true`/`false`/`nullptr` and string
  literals, id-expressions, parens) annotated with type, value category
  (3.10), and translation-time constant value (5.19).
- initializers (`= expression`) with the standard conversions (clause
  4), character-array initialization (8.5.2), and reference binding
  with lifetime-extended temporaries (8.5.3).
- `static_assert` declarations (7p4).
- array bounds generalized from one literal to a converted constant
  expression of type `std::size_t` (8.3.4).
- function definitions with empty bodies, overloads distinguished by
  signature (3.5), ODR checks (3.2).
- qualified declarator-ids switch the lookup scope for the rest of the
  declarator and the initializer (3.4.3p3), and must appear in an
  enclosing namespace (8.3p1) and redeclare an existing member.
- diagnosis of redeclaration/kind conflicts (3.3.1), namespace name
  conflicts (7.3.1), alias misuse (7.3.2), using-declaration conflicts
  (7.3.3).
- program-wide linking of external entities, then image layout and
  relocation.

## Ownership and data model

One `Program` (new `sema/program.h`) owns every linked entity,
lifetime-extended temporary, and string-literal object for the whole
program; each TU keeps its own `SemaModel` namespace tree, parsed in
command-line order by the same single forward pass as PA7, so
"declared before use", "preceding initialization" (5.19), and the
image's first-declaration order all fall out of creation order.

- `ImageSlot` (in `sema/entity.h`): the common base for everything
  that can be placed in the image or targeted by a relocation -
  entity, temporary, string literal. Holds the object type, the
  recorded initial value (one of ZERO / BYTES / ADDRESS{target slot,
  addend}), `init_is_constant`, `is_constexpr`, `defined_tu`, and the
  final image offset. A single scalar initializer means a slot never
  needs more than one relocation.
- `DeclaredEntity : ImageSlot` adds name, linkage, inline flag. The
  entity object is shared program-wide: when a TU declares an
  external name, `Program` returns the already-linked entity (keyed
  by home-namespace path + name, plus the parameter-type signature
  for functions). Internal-linkage entities are never keyed, so each
  TU gets its own. Constant reads check `defined_tu == current TU`,
  which makes cross-TU initializers correctly non-constant.
- `Binding` gains a function overload list (entity + per-overload
  imported flag) and an `imported` flag (set by using-declarations
  and namespace aliases) so redeclaration through an imported binding
  and namespace/alias conflicts are diagnosable typed state.
- Block lists: `Program` records Block 1 (entities, order of first
  declaration), Block 2 (temporaries, order of creation), Block 3
  (string-literal objects, token order; expression positions only -
  the static_assert message token is not an expression).

## New/changed files (dev/src/)

- `sema/type.{h,cpp}` += `TypeEquals` (structural), `TypeSize` /
  `TypeAlignment` (handout ABI table; overflow-checked array sizes),
  fundamental classification helpers, `RemoveTopCv`, strict factories:
  pointer-to-reference, reference-to-void, direct
  reference-to-reference (collapsing stays legal only through a
  typedef base, 8.3.2p6 - enforced in declarator composition), array
  of references/void/functions/incomplete arrays, function returning
  array/function all throw. `MergeRedeclaredType` now requires
  identical types modulo array completion (both directions) and
  throws otherwise.
- `sema/expr.{h,cpp}` (new): `Expr` annotation {type, value category,
  ConstValue}. ConstValue kinds: non-constant, integer (raw 64-bit +
  signedness from type), floating (long double), address (ImageSlot*
  + addend), null. Literal decoding from PostToken bytes, arithmetic
  conversion/encoding helpers (BYTES <-> value, symmetric), 5.19
  lvalue-to-rvalue (const integral with visible constant init, or
  constexpr object; references deref through their recorded constant
  binding), array-to-pointer and function-to-pointer decay, and the
  scalar standard-conversion driver of clause 4 (integral/floating
  conversions, qualification conversion 4.4 incl. multilevel rule,
  pointer conversions 4.10 with null pointer constants, boolean
  conversion 4.12, nullptr_t).
- `sema/init.{h,cpp}` (new): initialization semantics. Scalar
  copy-initialization (8.5p17) producing slot init facts; character
  array from string literal (8.5.2: element-type match, bound
  completion, length check, zero fill); reference binding (8.5.3p5:
  direct binding when reference-compatible, rvalue refs reject
  lvalues except functions, otherwise temp materialization via the
  same initialization recursion, only const non-volatile lvalue refs
  or rvalue refs may bind temps); static_assert evaluation
  (contextually-converted constant to bool); array bound evaluation
  (converted constant expression of size_t: integral only, narrowing
  /negative rejected, > 0).
- `sema/program.{h,cpp}` (new): `Program` (arenas, linking key map,
  TU index, block lists), entity creation/linking with cross-TU type
  and return-type consistency, linkage computation (3.5p3-4: static,
  unnamed-namespace closure, const non-extern), redeclaration linkage
  checks (static-after-external), ODR definition records (variables
  once per program; functions once unless inline, inline once per
  TU), layout (magic, Block 1 skipping never-defined variables,
  alignment padding per the handout - functions 4/4 - Blocks 2 and
  3), relocation (unplaced targets resolve to 0, matching the
  reference), and the image write.
- `sema/decl_parser.{h,cpp}`: pa8 grammar + semantic actions.
  Specifier state extended with storage-class/constexpr/inline facts
  and combination checks; declaration dispatch adds static_assert;
  simple-declaration handles init-declarators with initializers and
  the single-declarator function-definition form (`{}` body);
  expression parsing (primaries per pa8.gram, registering Block 3
  string objects in token order); array bounds via constant
  expressions; qualified declarator-ids resolve their namespace,
  check enclosure, and switch the scope chain for trailing suffixes
  and the initializer; declaration actions handle overload sets,
  kind/type/linkage conflicts, const/constexpr/reference
  definition-requires-initializer, incomplete-type definitions, and
  the namespace-definition/alias/using-declaration conflict rules.
- `dev/nsinit.cpp`: real driver replacing the stub - shared
  predefined macros, one `Program`, per-TU pipeline + `SemaModel` +
  `DeclParser` on the large-stack worker (as nsdecl), then layout and
  binary image write; any error exits EXIT_FAILURE.
- `dev/frontend_source_sets.mk`: nsdecl += expr/init/program (shared
  DeclParser needs them); nsinit = the nsdecl set.

nsdecl keeps PA7 behaviour byte-for-byte: it builds a throwaway
`Program` per TU, so linking never crosses TUs, and the added
diagnostics only fire on inputs that are ill-formed (UB for PA7 and
absent from its fixtures - checked against the pa7 local and course
suites via the through report).

## Semantic decisions pinned by fixtures/handout/reference probes

Fixtures gate; on non-fixture inputs the handout and standard win over
reference parity (AGENTS.md). Probes of `nsinit-ref` informed these:

- Non-constant initializers: the handout says write zero; the
  reference errors "unsupported non-constant scalar initializer" on
  e.g. `int y; int x = y;`. Follow the handout: accept, zero-init,
  and mark non-constant (so static_assert/bounds/constexpr that need
  the value still error).
- Function stub alignment: handout says mock alignment 4; the
  reference appends stubs unaligned (probe: stub at offset 5). No
  fixture discriminates; follow the handout (align 4).
- Undefined extern variable used as a relocation target: reference
  emits offset 0 and succeeds (ill-formed NDR). Match the reference.
- Block ordering probes: temporaries (creation order) precede string
  literals (token order); each string-literal token is its own object
  (no dedup); a reference bound to a string literal binds directly
  (the reference also emits a dead temporary there - artifact, not
  copied); string literals used to initialize arrays still get Block
  3 objects; the static_assert message token does not.
- `static_assert(1, 42)` (non-string message): accepted by the
  reference and by pa8.gram (TT_LITERAL); accept any literal token.
- Long double constants: write the 10 value bytes + 6 zero padding
  bytes (the reference leaks garbage there; no fixture has a
  long-double constant).
- `int a[true]`: accepted (converted constant expression), matching
  the reference.
- inline variables rejected (7.1.2: inline is for functions; the
  reference accepts - standard wins). constexpr functions accepted as
  declarations and stub definitions (reference accepts; the C++11
  body rules are unenforceable on `{}` bodies and irrelevant here),
  but constexpr/non-constexpr redeclaration mismatch errors (7.1.5p2).

## Validation

1. `make test-report ACTIVE_TEST_REPORT_PAS='pa8'` while iterating
   (41 local fixtures).
2. `make test-report-through-pa8` as the exit gate (pa1-pa7 must stay
   green; nsdecl shares the sema sources, so regressions show here).
3. `perl scripts/cppgm_file_audit.pl --stage pa8 --paths dev/src`.
4. Differential sweep against `nsinit-ref` (~85 synthesized inputs:
   multi-TU linking orders, overloads, reference chains, conversions,
   qualification rules, arrays/strings, static_assert, namespace and
   redeclaration conflicts, qualified declarator scope, internal/
   external linkage mixes). All checked divergences triaged below.

## Known reference divergences (non-fixture inputs)

Fixtures gate the assignment; on non-fixture inputs the handout and
the standard win (AGENTS.md). The sweep found these classes:

- Reference value bugs: reading a constexpr/known pointer variable
  yields the address *of the variable* in the reference's output
  (`const char* t = s;` gives `&s`, not s's value - inconsistent with
  its own 500-static-assert3 handling); `3.14f` is emitted as 3.0f.
  We emit the IEEE/5.19-correct values.
- Reference "unsupported" limitations (it errors, we implement the
  standard semantics): non-constant scalar initializers and
  non-constant temporaries (handout: write zero), rvalue-reference
  temporary binding (`int&& r = 3;`), temporaries of pointer type
  (`const char* const& r = "abc";`), taking the address of a function
  that later gains overloads, qualified redeclaration with a
  qualified typedef specifier (`B::Q B::x = 7;` inside an enclosing
  namespace), floating/function static_assert conditions
  (`static_assert(0.5, ...)` - well-formed, g++ agrees).
- Reference leniencies the standard rejects (we diagnose, g++
  agrees): `int x = nullptr;`, overloads differing only in return
  type, typedef redefinition to a different type, zero array bounds,
  `static extern int x;`, `typedef static int T;`, static after
  external declaration, storage classes in parameters, initializers
  on function declarators, volatile-const reads in constant
  expressions, inline variables.
- Permitted-optimization folding: the reference constant-folds reads
  of non-const variables whose value it knows (`int x = 1; int y =
  x;` gives y=1) - legal under 3.6.2p3 but inconsistent with its own
  "unsupported non-constant" errors; we apply 5.19 strictly and
  zero-initialize (the handout's "otherwise write zero").
- Linking of internal-linkage functions: the reference links two
  `static void f()` from different translation units into one stub
  and accepts cross-TU double definitions of non-inline functions; we
  keep internal entities per-TU (3.5p3, as the 130-staticvar fixture
  does for variables) and diagnose cross-TU redefinition (3.2, what
  real linkers do). Cross-TU declared-type mismatch (`extern int x;`
  / `extern double x;`) is ill-formed NDR; we diagnose.
- Function stub alignment and the dead temporary on string-literal
  reference binding, per the decisions above.
