# PA36 Plan: Hosted Header Emission Link/Runtime Compatibility

Status: in progress (loop 81 start: 36/69 pa36 link tests passing).

PA36 asks whether hosted header-generated code (inline, template, and
header-emitted definitions) links and runs through the plain host
toolchain. The compile surface is owned by PA34/PA35; the host object
and ABI/runtime path by PA32/PA33. PA36 owns symbol ownership, host ABI
spelling, and runtime behavior of the emitted objects.

## Failure triage (loop 81)

33 failing tests cluster into these root causes:

1. **Host ABI spelling: standard abbreviations in prefixes.** We
   already spell `Ss/Si/So/Sd/Sa/Sb` when the abbreviated entity is a
   complete type spelling (`MangleComponentList`, parts.size()==2), but
   never inside nested-name prefixes (`ManglePrefixComponents`, longer
   component lists). So a member of `std::basic_ostream<char>` spells
   `_ZNSt13basic_ostreamIcSt11char_traitsIcEE9_M_insertIlEE...` while
   libstdc++ exports `_ZNSo9_M_insertIlEERSoT_`. Every reference to a
   library-exported member of an abbreviated specialization misses.
   Affects: ostream `_M_insert`, stringbuf/stringstream/ostream-chain
   smokes, and any signature spelling `RSo`/`RSs`.

2. **extern "C" inside a namespace.** `lower_unit.cpp` clears/sets
   `object_name` for c-linkage entities from `HostLibraryBuiltinSymbol`
   only; a non-builtin falls back to the LowIR scope-path name, so
   `__cxxabiv1::__cxa_allocate_exception` emits as
   `__cxxabiv1____cxa_allocate_exception`. 7.5: a C-linkage entity's
   symbol is its unqualified source name regardless of namespace.

3. **GNU `__asm` labels.** PA34 parses and discards
   `declarator __asm("label")` (ast_parser_core.cpp). PA36 must use the
   label (adjacent string literals concatenated) as the object symbol
   for references and definitions of that declaration.

4. **Typedef name for linkage (7.1.3p9).** `typedef struct {...}
   __mbstate_t;` names an unnamed class; we mangle the internal
   `__local_typeN` placeholder. The first typedef name is the linkage
   name: `std::codecvt<char, char, __mbstate_t>` must spell
   `11__mbstate_t`. Affects `__basic_file`, `codecvt`, filebuf-based
   iostream tests.

5. **Demand-closure gaps.** Emitted header definitions reference
   instantiations that were never emitted (`_Rb_tree::_M_copy` second
   overload, `__copy_move_a2`, `std::uninitialized_copy`): undefined
   locals at link. Needs investigation - likely the required-definition
   closure keys instantiations in a way that collapses overloads of one
   member template, or drops callees discovered only while emitting.

6. **Inline header definitions emitted strong.** The shared-ptr ODR
   test defines `std::__exception_ptr::exception_ptr::~exception_ptr()`
   in plain `.text` with strong binding in two TUs: multiple
   definition. Hosted inline (non-template) member functions need
   weak/comdat emission like template instantiations already get.

7. **extern const cross-TU.** `extern const` namespace variable defined
   in one TU, referenced from another: we internalize const objects
   unless the defining declaration itself is `extern` - need to honor
   the earlier extern declaration when a later definition omits it.

8. **Relocation classes.** Object-inspection tests expect GOT-mediated
   loads (`imported_data_got`) and PC-relative data relocations
   (`data_pcrel`) for imported globals; our emission uses a different
   relocation shape there.

9. **Semantic/instantiation bugs.** `_Hashtable` member lookup fails
   (`no member named _M_hash_code` - lookup through the
   `_Hashtable_base`/`_Hash_code_base` non-dependent base chain),
   dependent-alias-pack `invoke_result`, plus several runtime crashes
   (std::function call, vector<string>, iostream move-assign) to be
   re-diagnosed after the symbol-spelling fixes land.

## Design

### Standard abbreviations (fix 1)

The abbreviation catalog lives in `lower_name.cpp`
(`StdSpecializationAbbreviation`, `StdTemplateAbbreviation`,
`SpellStdMemberComponent`) keyed on typed component facts (template
identity, declaring scope, argument types) - not on spellings. Extend
use to the two remaining spelling paths:

- `ManglePrefixComponents`: when the head is `::std` and the second
  component abbreviates, spell the abbreviation for the [std, spec]
  pair and continue with the remaining components. A whole-spec
  abbreviation (`So`) is a direct substitution: it does not register a
  numbered candidate for itself; later components still register their
  full chain keys, and those chain keys stay structural (identical to
  the unabbreviated keys) so compression is unaffected.
- `MangleComponentList` (general nested path): same [std, spec] head
  handling for parts.size() > 2.

`Sa`/`Sb` template-name abbreviations spell `SaI...E` and the
template-id still registers its full key (already the behavior in
`SpellStdMemberComponent`; reuse it).

Ownership: the mangler derives everything from resolved semantic
components; no hosted-only side channel.

### extern "C" object names (fix 2)

In `lower_unit.cpp`, c-linkage entities take
`object_name = <unqualified source name>` in separate-compilation mode
(host parity is mode-gated per PA32); whole-program mode keeps the
pinned LowIR spellings. Builtin `__builtin_*` redirections keep
`HostLibraryBuiltinSymbol` precedence.

### GNU asm labels (fix 3)

- Parser: `ast_parser_core.cpp` records the concatenated string-literal
  value of a post-declarator `__asm("...")` on the declarator instead
  of skipping it (AstDeclarator gains `asm_label`).
- Sema: the decl binder records the label per declared overload on the
  ScopeBinding (parallel to `fn_c_linkage`), and the sem-node item for
  the declaration carries it to lowering (typed state, not string
  re-derivation).
- Lowering: an asm label overrides `object_name` for the function
  entry (references and definitions alike). Labels are verbatim ELF
  symbol names on this target (no user-label prefix).

### Typedef linkage names (fix 4)

`NamedTypeInfo` gains `linkage_name`, set by `BindTypeAlias` when the
aliased type is an unnamed class/enum entity (its `__local_typeN` /
`__anonymous_*` name is display-only) that has no linkage name yet and
the typedef declares it directly (no cv, no derived types). The mangler
component builders (`ScopeComponent`, `EntityComponents`) spell
`linkage_name` when set. Display names and PA12 fixtures stay
unchanged.

### Later fixes (5-9)

Re-triage after the spelling fixes land; each gets its own commit.
Priorities: demand closure (5) and weak emission (6) are emission-model
work owned by lowering/object layers; relocation classes (8) belong to
the x86 encoder; semantic bugs (9) go wherever the defect is (lookup,
instantiation).

Progress notes (loop 81):
- Fixes 1-4 (symbol spellings) committed; pa36 33 -> 25 failures.
- Demand-closure gaps (5) root-caused and committed, three parts:
  probe traits (__is_trivially_copyable family) reached from the PA11
  constant-expression walker now route to the analyzer via
  IConstExprContext::ProbeTraitConstant (hosted __memcpyable enum
  initializers were silently poisoning instantiations); the retry and
  pending-instantiation passes alternate until both queues drain; and
  out-of-class member-template definition pairing accepts the
  self-qualified return spelling (9.3p5) with the definition's renamed
  outer parameters kept resolvable through the replay alias scope
  recorded as the member template's lookup scope. pa36 at 46/69.
- Fix 6 (inline emission): out-of-class spelled-inline destructors now
  carry spelled_inline (BindQualifiedDestructor), fixing the strong
  `.text` exception_ptr dtor ODR clash. Fix 7 (extern const): a prior
  extern declaration keeps a later const definition external
  (LowGlobalInfo.extern_declared). Also: a folded constant lvalue
  (std::string::npos bound to a reference) materializes into a local
  slot instead of failing the PA14 address boundary.

Remaining failures by root cause (loop 81 end-state triage):
- unordered_map: `_M_hash_code` member lookup through the
  `_Hashtable_base`/`_Hash_code_base` chain fails; unordered_set now
  clears the invocability assert (functor calls demand completeness
  before the operator() lookup) but trips the nothrow one - the
  ProbeTraitInvocable no_throw leg for the hash<T*> partial-spec
  member is next.
- `basic_streambuf does not name a type` during <streambuf> member
  instantiation (header-inline-unemitted-callee-signature).
- `_M_erase_at_end does not name a type`: statement decl/expr
  disambiguation inside instantiated deque bodies (deque-move-assign).
- vector<Box> brace-init: initializer_list constructor not viable
  (vector-class-brace-init-ref-capture).
- Function-local static/thread_local with destructor outside the PA14
  boundary (inline-thread-local-deque-destructor-once); needs
  __cxa_atexit/__cxa_thread_atexit registration in lowering.
- Runtime crashes (139/134): std-function-call,
  vector-string-substitution, iostream-move-assign,
  iostream-runtime-symbol, ostream-reference-getloc-vbase,
  vector-bool-move, plus stdout mismatches (global-istringstream-init,
  ostringstream-tellp, stringstream-insertion).
- Relocation-class inspections: imported-global-got-load (GOT),
  pcrel-data-reloc.
- ofstream inspect: extern-template member ownership (fix planned as
  ClassSpecialization extern_declared suppression, task not started).

## Validation

- Fast loop: `make check TEST=tests/link/<case>.t` inside pa36, and
  `make test-report ACTIVE_TEST_REPORT_PAS='pa36'` at the root.
- Gate: root `make test-report-through-pa36` (older stages are part of
  the bar; mangling changes especially must hold PA30-PA35 steady).
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa36 --paths dev/src`.
