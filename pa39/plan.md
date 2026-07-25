# PA39 Inception Plan

PA39 adds no new compiler surface. The work is: make the existing
`../dev/cppgm++` rebuild every checkpoint tool from `frontend_source_sets.mk`
(`*-self`), keep the PA1-PA38 preservation ladder green under the self-built
checkpoints, then prove reproducibility (`*-self` rebuilds itself into a
byte-identical `*-inception`). Every ladder failure is treated as an earlier
compiler bug or a reproducibility bug until proven otherwise.

## Current checkpoint: pptoken-self (PA1 rung)

`make -C pa39 test-through-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++` fails
in the very first rung: host-seeded `../dev/cppgm++` cannot compile four of
the pptoken checkpoint sources.

### Failure 1: "unterminated comment" (source_translation, pp_tokenizer, pptoken)

Comments in `dev/src/source_translation.h` (included by all three failing
TUs), `dev/src/source_translation.cpp`, and `dev/src/ctrl_expr.cpp` spell the
raw character sequence `\UFFFFFFFF` while documenting the end-of-input marker.
The course translation pipeline decodes universal-character-names during
phases 1-2, before comments are stripped, and `\UFFFFFFFF` wraps to the
end-of-input marker, cutting the file off inside a `//` comment.

This is **not** a compiler bug: `pptoken-ref` rejects the same file at the
same spot (`ERROR:9:73:Unterminated comment` on source_translation.h), so the
source text is invalid under the course dialect for any conforming course
compiler, host-seeded or self-built. Per the working rules, a source rewrite
is the correct fix exactly because the source is wrong under host-seeded
`../dev/cppgm++` (and the reference) too. Fix: reword the three comments so
they no longer contain a raw UCN spelling; behavior of the compiled code is
unchanged. A repo sweep found no other raw `\u`/`\U` + hex sequences in
`dev/`.

### Failure 2: "opaque unscoped enum declaration" (test_runner)

`dev/src/test_runner.cpp` includes `<fcntl.h>`; glibc's `struct f_owner_ex`
declares the member `enum __pid_type type;`. `DeclBinder::BindEnum`
(`dev/src/sema/decl_enum.cpp`, PA11 7.2 surface) treats every bodyless,
baseless, unscoped `DK_ENUM` as an opaque-enum-declaration and throws — but
an elaborated-type-specifier `enum E` used with a declarator merely refers to
a previously declared enumeration (7.1.6.3p3 / 3.4.4p2). `cppgm++-ref`
accepts `enum E e;` members, `enum E x = A;` declarations, and `enum E f();`
declarators, and compiles the `<fcntl.h>` probe; it still rejects standalone
`enum E;` (with or without a prior declaration) and elaborated references to
undeclared enums.

Real compiler bug, earliest owning surface PA11 (the enum entity model in
`sema/decl_enum.cpp`; the parse side already accepts the form). The PA7/PA8
`nsdecl` model has no enum support at all and none of its fixtures exercise
enums, so PA11 is the earliest surface that owns this behavior. Fix: give
`BindEnum` an `elaborated` mode (mirroring `BindClassForward`) that resolves
the name by unqualified lookup and requires an enumeration; wire it from
`BindNestedTypeSpecifier`. Reducer: new course tests under
`cppgm.tests/course/pa11/` with fixtures regenerated via
`make -C pa11 ref-test TEST=course/pa11/<test>.t`.

### Failure 3: PIE host links reject our objects (pptoken-self link)

With both compile failures fixed, the `pptoken-self` host link failed:
`ld` rejects our objects under the default-PIE host `g++`
(`R_X86_64_PC32 against __gxx_personality_v0 can not be used when making
a PIE object`, plus DT_TEXTREL warnings from movabs address
materialization). The reference compiler's objects link and run under
default PIE, so this is a real hosted-emission gap that the earlier
interop harness masked by always linking `-no-pie`
(`run_cpphostinterop_tests_worker.pl`). Three PA36-surface fixes, all
gated on host-object mode:

- `x86/lowir_to_mir_*`: taking the address of a function the unit only
  declares now loads from the GOT (`imported_function_global`),
  matching the host GOTPCREL spelling; direct calls keep PLT.
- `x86/mir_to_native.cpp`: OP_SYMBOL (defined-function) address movs
  materialize rip-relative like OP_GLOBAL, not movabs.
- `toolchain/elf_object.cpp`: the CIE personality is encoded
  indirect-pcrel (0x9b) through a DW.ref-style 8-byte .data slot
  holding the absolute `__gxx_personality_v0`, exactly the host
  convention. `toolchain/elf_reader.cpp` recovers the indirect form
  (verifying the slot really names `__gxx_personality_v0` so foreign
  personalities stay skipped), and the private runtime library defines
  `__gxx_personality_v0` so the slot resolves in private links.

Reducers: `cppgm.tests/course/pa36/link/700-hosted-imported-function-
addr-got-link-smoke` and `700-hosted-eh-personality-indirect-link-smoke`
pin the GOT spelling and the absence of a direct personality relocation
via `.inspect.expect`; the reference passes both.

Note: root `make test-strict` fails pre-existing (`--witness` is
rejected by the driver in emit modes for pa18-pa23); unrelated to and
unchanged by this work, and not part of the PA39 required checks.

### Failure 4: ctor-template specialization poisoned before its
### out-of-class definition (pptoken-self link, `_Hashtable` ctor)

With PIE fixed, `pptoken-self` linking failed on an undefined
`_Hashtable::_Hashtable<_InputIterator>(..., true_type)` — libstdc++'s
tagged range constructor, defined out of class. Two poisons in the
PA21 member-template surface (`sema/sem_member_template.cpp`):
candidate synthesis marked specs of definition-less ctor templates
`body_emitted` permanently, and `InstantiateCtorTemplateBody` marked
`body_emitted` before checking whether the pattern had a body. When a
class instantiation's out-of-class member definitions replay *after* a
member body already delegated into the tagged constructor (exactly
libstdc++'s ordering), the spec stayed body-less forever and the call
resolved to a never-defined weak symbol. Fixed by recording the demand
(`odr_used`, mirroring `OnSpecializationOdrUsed`'s use-before-
definition path) and binding demanded bodies when the definition
pairing completes. Also: the float/negmask constant pool addressed
memory with absolute `[disp32]` (`R_X86_64_32S`), rejected by PIE;
host objects now use rip-relative pool addressing (`PoolMem`).

Reducer: `cppgm.tests/course/pa36/link/600-hosted-unordered-set-init-
list-link-smoke` (the ordering needs hosted libstdc++, so the hosted
link suite is the earliest harness that can express it).

### Failure 5: loop-live values kept in caller-saved registers (pa1
### self tests, pptoken-self miscompiled)

`pptoken-self` built but failed every PA1 test, segfaulting or dropping
tokens after the first new-line. Reduced (freestanding `tok1.cpp`, then
raw LowIR): `MarkCallCrossings` in `x86/lowir_to_mir_analyze.cpp` used
the *linear* (def, last-use) window to decide `crosses_call`, so a
value defined before a loop and read at the loop head — with the loop's
call linearly *after* the last use — was housed in a caller-saved pool
register (r8) and clobbered every iteration. Latent since PA28, exposed
by PA37's -O2 LowIR CSE hoisting `index`-derived addresses out of
loops; mode-independent (whole-program and hosted alike). The reference
backend houses such values callee-saved. Fix: extend each value's
effective range through every loop region (backedge target start →
backedge block end, to fixpoint) it is live into; the old behavior is
unchanged for values that cross no backedge, so the pinned PA28/PA38
MIR fixtures stay byte-identical (report re-run green).

Reducer: `cppgm.tests/course/pa28/300-loop-live-value-across-call.t`
(behavior kind: raw LowIR loop with the call in the loop body and the
linearly-last use before it).

## Validation plan

1. `make -C pa39 probe-self-object SOURCE=...` on each previously failing TU.
2. Reducer tests pass; `make test-report-through-pa38` stays green.
3. `make -C pa39 test-through-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++`
   (the blocker) — iterate on the next first failure this exposes, same
   method: reduce, find owning surface, fix there, add reducer.
4. `make -C pa39 compare-pptoken-inception ...` as the first reproducibility
   compare, then the full `compare-cppgm++-inception` target.

## Ladder expectations

Self-built checkpoints must behave identically to host-seeded builds on the
same inputs; >5x slowdowns, timeouts, or OOM in `*-self`/`*-inception`
compiles are layer divergence to trace back to a miscompiled self compiler,
not something to tune around. No self-hosting special cases, no generated
source discovery, no harness weakening.
