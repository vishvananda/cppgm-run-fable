# PA36 Plan: Hosted Header Emission Link/Runtime Compatibility

Status: in progress (loop 83: 63/69 pa36 link tests passing,
through-pa36 at 3334/3340 with every failure inside pa36). Landed
this loop: dynamic exception specifications (15.4/15.5.2 LSDA
filters + __cxa_call_unexpected), the host data-addressing model
(pcrel data access, GOT-mediated imports, course _Z spelling for
unscoped non-TLS variables), and destructible function-local statics
(__cxa_atexit/__cxa_thread_atexit registration, weak/TLS guard
mirroring, lowering/lower_static.cpp).

PA36 asks whether hosted header-generated code (inline, template, and
header-emitted definitions) links and runs through the plain host
toolchain. The compile surface is owned by PA34/PA35; the host object
and ABI/runtime path by PA32/PA33. PA36 owns symbol ownership, host ABI
spelling, and runtime behavior of the emitted objects.

## Design decisions landed (loop 82)

1. **Virtual-base conversions on non-complete objects (host mode).**
   Pointer/reference conversions into a shared virtual base used the
   static complete-object offset (or nothing for raw pointer values).
   Host mode now reads the offset from the installed vtable header for
   polymorphic classes: `AdjustToBase` and `AdjustPointerToVBase`
   (null-guarded) ride `DynamicVBaseAddress` (lower_vbase.cpp), and
   polymorphic classes stop carrying hidden `__pvbptr` params entirely
   (`AppendParamCarried` gates on separate compilation), so their
   signatures match the host ABI. Whole-program keeps the PA27 carrier
   model and pinned shapes. Fixed the iostream move-assign abort.

2. **Operator target deduction (13.4/14.8.2.2).** `ResolveOperatorCall`
   runs `DeduceFunctionSetArguments` like the call path, so
   `cout << endl` deduces `endl<char>` against the manipulator
   parameter. A function set no candidate resolves reports cleanly.

3. **Extern-template ordering (14.8.2.6).** Deduction from an
   `extern template` declaration's declared signature reduces multiple
   matching templates by partial ordering, so the char-stream
   `operator<<` overload suppresses correctly.

4. **Instantiated members keep vague linkage (14.7.1).** Out-of-class
   defaulted special members of specializations defer weak-on-demand;
   an instantiated out-of-class destructor no longer anchors the
   specialization vtable strong; an initializer-less explicit
   specialization of a static data member is a declaration
   (14.7.3p15), leaving `_S_timezones` to libstdc++.

5. **Extern-template class declarations (14.7.2p10).** Non-inline
   member definitions (functions and statics) of an extern-declared
   specialization do not instantiate; references spell the external
   symbol. The suppression is reversible (members stay pending until
   an explicit-instantiation definition lifts the flag). In-class
   (inline) members keep local weak emission per the standard's
   carve-out — see "ofstream inspect" below for the blocked follow-up.

6. **Qualified friend templates + 11.4 protected access.**
   `template<...> friend struct __detail::_Map_base;` resolves through
   its prefix scope and grants every specialization, unblocking
   `_Hashtable`'s protected `_M_hash_code` chain (unordered_map).

7. **Conditional noexcept (15.4p1).** `noexcept(constant-expression)`
   specifications reduce through the constant machinery via a new
   `ITypeBuilderHost::EvaluateNoexceptSpec` hook; abstract patterns
   defer to instantiation-time recomposition (unordered_set's
   `_M_bucket_index` static_assert).

8. **Vexing-call recovery + 13.3.1.7.** `f(g());` recovers as a call
   when `f` is not a type (deque::clear), and braced non-aggregate
   class initialization runs the initializer-list-constructor phase
   first (`SelectListCtorInit`, materializing the list through the
   existing braced-argument conversion machinery).

9. **Empty if-branches.** `if (x) ; else ;` binds empty branch
   wrappers; LowerIf accepts them (vector<bool> move ctor).

## Loop 83: dynamic exception specifications (15.4, 15.5.2)

`600-hosted-dynamic-exception-spec-runtime` fails its inspect: the
object never references `__cxa_call_unexpected` because `throw(T...)`
specifications are parsed (AST `throw_types`) and then dropped by
sema. Design, ownership boundaries by layer:

- **Sema owns the spec as typed state.** `DeclaratorInfo` gains
  `throw_spec_types` (resolved via `ResolveTypeId`, with the 15.4p2
  array->pointer / function->pointer adjustment); `SemNode` carries it
  on `SN_FUNCTION_DEFINITION` (copied at the namespace-scope, member
  -body, and instantiated-body build sites, beside `unwind_no`).
- **Lowering (host mode only) arms a whole-body filter region**, the
  same shape as the 15.4p9 noexcept terminate region: `eh_try
  ^eh_spec_dispatch` after parameter stores; the dispatch block
  publishes an `eh_filter @_ZTI...` marker (spec types by
  `ThrowRttiRef`, references stripped like handlers) and calls
  `__cxa_call_unexpected(exception ptr)` (new external runtime ref).
  `EmitUnwindLeave` — the single frame-escape funnel — branches on
  `exception_selector < 0` (the personality's filter-match switch
  value) to an inlined call-unexpected route, else resumes; this
  avoids the same resume-loop hazard the terminate region documents.
  Whole-program mode keeps its pinned region-free shapes (specs stay
  inert there, as before).
- **MIR region analysis** turns `eh_filter` markers into `HC_FILTER`
  clauses (type-symbol list already modeled on `HostEhClause`).
- **LSDA**: `EhAction` gains `EH_SPEC`. Encode assigns ttype slots
  above the catch filters to spec types, appends the null-terminated
  uleb spec lists after ttbase, and emits the action filter as
  `-(spec offset + 1)`; decode reads them back symmetrically
  (`spec_filters` -> `elf_reader` rewrites to symbols like catch
  entries). The private-link table builder ignores `EH_SPEC` records
  (the private runtime has no unexpected support; matches the
  previous ignore-spec semantics of whole-program mode).

Runtime behavior comes from the host: `__gxx_personality_v0` treats a
thrown type absent from the spec list as a handler match with a
negative switch value, and `__cxa_call_unexpected` runs the
`std::unexpected` handler, re-checks any replacement exception
against the cached spec, and propagates/terminates per 15.5.2.

## Remaining failures (loop 82 end triage)
- `600-hosted-std-function-call-link-smoke` (prog 139) and
  `600-hosted-vector-string-substitution-link-smoke` (prog 139):
  runtime crashes in emitted header code; re-diagnose with gdb next
  (the vbase fixes changed the landscape; old notes are stale).
- `700-hosted-global-istringstream-init-link-smoke`,
  `700-hosted-ostringstream-tellp-runtime-smoke` (now 139),
  `700-hosted-stringstream-insertion-runtime-smoke`: stringstream
  runtime behavior/stdout; likely one shared stringbuf defect.
- `700-hosted-imported-global-got-load-link-smoke` and
  `700-hosted-pcrel-data-reloc-link-smoke` (impl 255): object
  inspections expecting GOT-mediated loads (`imported_data_got`) and
  PC-relative data relocations (`data_pcrel`) for imported globals;
  belongs to the x86 encoder's relocation-class selection.
- `700-hosted-inline-thread-local-deque-destructor-once` (impl 1):
  function-local static/thread_local with destructor needs
  __cxa_atexit/__cxa_thread_atexit registration in host lowering
  (currently outside the PA14 boundary error).
- `700-hosted-ofstream-file-runtime-smoke` (impl 1): the inspect wants
  in-class members of extern-template classes (basic_ofstream's
  constructors) referenced externally like GCC emits them. Blocked on
  per-constructor lowering entry identities: a converting-constructor
  -template specialization (allocator<char>'s) shares the plain copy
  constructor's signature, so the shared scope+name+type entry
  declares the wrong symbol when suppressed. The facts are recorded
  (SemUnit::extern_class_scopes, SemNode::member_template_body,
  LowerProgram::extern_member_scopes_); the consumption site in
  MemberFunctionEntry documents the constraint. Related latent issue:
  the same shared entry means the selected plain copy ctor and the
  ctor-template instance overwrite each other's member_defs_ slot.

## Validation

- Fast loop: `make check TEST=tests/link/<case>.t` inside pa36, and
  `make test-report ACTIVE_TEST_REPORT_PAS='pa36'` at the root.
- Gate: root `make test-report-through-pa36` (older stages are part of
  the bar; mangling changes especially must hold PA30-PA35 steady).
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa36 --paths dev/src`.
- Debugging technique (the audit forbids committed env hooks): to see
  host-mode LowIR, temporarily dump `task.lowir_text` in
  `LowerUnitToLowIR` (compile_unit.cpp); to see swallowed
  instantiation errors, temporarily print the caught exception in
  `RetryDeferredBodies` (sem_template_check.cpp) and
  `FlushDeferredBodies` (sem_member_body.cpp). Remove before commit.
