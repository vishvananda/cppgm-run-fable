# PA37 Plan: `lowiropt` LowIR Optimization Stage

## Scope

Three connected pieces, all built on the existing PA13 LowIR object model
(`dev/src/lowir/lowir_program.h`, parser, validator):

1. The `lowiropt` tool: parse LowIR text, run the deterministic pipeline at
   `-O0`/`-O1`/`-O2`, write the canonical dump. Gated byte-exactly by
   `pa37/tests/o0`, `o1`, `o2`.
2. The driver surface: `cppgm++ --emit-lowir -g0 -O<n>` runs the hosted
   separate-compilation lowering (the same LowIR text `-c` compiles), then the
   same optimizer, then the same canonical dump. Gated by `pa37/tests/driver`.
3. The object boundary: `cppgm++ -c` accepts serialized LowIR input, and both
   `-c` paths (source and LowIR input) run the same optimize-then-object
   pipeline so `--emit-lowir -g0 -O0` + `-c -O<n>` reproduces the direct
   `-c -O<n>` object byte-for-byte. Gated by `pa37/tests/object-roundtrip`.

## Ownership boundaries

- `dev/src/lowir/lowir_dump.{h,cpp}`: canonical `LowIRProgram` -> text
  serializer. Presentation rules (owned here, used by lowiropt and the driver
  emit path):
  - top-level phase order: `declare global`, `declare function`, `global`,
    `function`, `alias object`; input order preserved within each phase
  - blank line after the declare/global phases; function bodies printed
    back-to-back; slots then a blank line before the first block; blank line
    between blocks
  - operand/metadata spellings preserved as parsed (literal spellings are
    kept verbatim; folded float results introduce `inf`/`-inf`/`nan` only)
  - a definition carrying `prefer_local=yes` with no explicit binding prints
    `binding=strong` inserted before `prefer_local` (reference parity)
- `dev/src/lowir/lowir_opt.{h,cpp}` + `lowir_opt_*.cpp`: the optimizer.
  Owns a per-function CFG view (block -> instruction ranges, successor,
  predecessor, EH region structure) rebuilt after structural passes.
- `dev/lowiropt.cpp`: CLI only (already scaffolded): argument parsing, file
  IO, parse -> validate -> optimize -> dump.
- Driver/`-c` integration stays in `dev/cppgm++.cpp` +
  `dev/src/toolchain/compile_unit.cpp`; the optimizer is invoked on the
  parsed program before MIR lowering.

The optimizer works only on the parsed `LowIRProgram`; no frontend side data
crosses the text boundary (PA37 contract). Facts the object layer needs after
optimization (inferred no-unwind for defined functions) are re-derived from
LowIR bodies by a small analysis shared with the optimizer.

## Pipeline (reverse-engineered from the byte-exact refs)

Program-level driver: process functions in program order; repeat rounds until
no change (inline uses the current state of callee bodies, so later-in-file
callees may force a second round — `400-inline-second-round-eh-wrapper`).

Per function, iterated to a local fixpoint:

1. **Value pass (SCCP-style, executable edges)** over temps (and slot contents
   at `-O2`): constant folding for `copy`/`unary`/`binary`/`cmp`/`convert`,
   algebraic identities (`x+0`, `x*1`, `x&-1`, identity `convert`,
   `unary decay ptr` removal, `cmp` of identical operands), boolean compare
   cleanup (`cmp eq/ne` vs 0/1 over an i64 bool), local reassociation of
   constant chains (result keeps the *last* temp's name), copy/constant
   propagation, `branch`/`switch` selector folding (drives edge executability).
2. **Pure-expression reuse (CSE)**: forward dataflow of available syntactic
   expressions (`addr`, `index`, `unary`, `binary`, `cmp`, `convert`) with
   intersection at joins over executable edges; commutative operand
   normalization and reversible compare directions map to one key; `const` is
   never reused (`storage-temp-constptr-guard`). Exceptional edges: a handler
   only receives expressions available at the `eh_try` point.
3. **Inlining**: direct calls to defined, non-recursive (no call-graph cycle)
   callees within the size bar (generous; `prefer_local=yes` raises it).
   Naming contract: per-caller site counter N, every inlined temp/label/slot
   gets prefix `__o1inl<N>__`. Single-block callees paste inline; multi-block
   callees split the call block into `^__o1inl<N>__entry` (via jump) and
   `^__o1inl<N>__cont`, with returns becoming stores to a
   `$__o1inl<N>__retmerge__1` / `$__o1inl<N>__retmergeobj__1` slot when the
   callee has multiple returns. Param-spill slots of the pasted body forward
   to the argument values and disappear. Obj returns (`return obj $slot`)
   substitute the returned slot into the consuming `copyobj`. Calls inside an
   EH region inline only when the callee body contains no operation that may
   transfer to a handler (no throw/resume, and no calls unless spelled
   `unwind=no`); calls inside handler/landing-pad code never inline.
4. **CFG cleanup**: remove unreachable blocks, bypass trivial jump-only
   blocks, merge single-pred/single-succ pairs, collapse empty branch
   diamonds. Guards: never merge across an open `eh_try` region tail, into a
   landing-pad target, or around `eh_cleanup`/`eh_end` structure.
5. **DCE**: unused pure temps; unused calls only when `readnone` +
   `unwind=no` + not `noreturn` (direct or via indirect signature); unused
   direct slot loads; stores to slots with no remaining loads or escapes;
   all-or-nothing same-block store-to-load slot forwarding (single store, all
   loads after it in the same block, no intervening may-write); unused slot
   declarations. `-O1` keeps cross-block slot traffic intact.
6. **EH strip**: in a function spelled `unwind=no`, remove `eh_try`/`eh_end`
   pairs whose protected region contains no operation that can reach the
   handler, then drop the dead handler blocks.
7. **`-O2` promotion**: optimistic executable-edge tracking of non-escaping
   scalar/`ptr` slots accessed only by direct `load`/`store`; a load folds to
   the tracked value only when all executable predecessors agree (no phi
   creation); may-throw points inside a region merge the current slot state
   into the handler's in-state (`eh-slot-merge-promotion-guard`); dead stores
   to promoted slots are removed when no observable load (including via the
   exceptional edge) can see them.

Names are preserved: surviving instructions keep their input temp names; CSE
keeps the first definition's name; folded literals substitute in place.

## Driver deltas (phase 2/3)

- `--emit-lowir` with a `-g` flag = driver surface: hosted preprocessing,
  `SetSeparateCompilation()`, benign driver flags accepted. The pa14-pa27
  whole-program `--emit-lowir -O0` fixtures (no `-g`) keep the existing path.
- The separate-compilation lowering stops printing *inferred* `unwind=no`
  (declared/deduced noexcept only). The MIR layer re-derives inferred
  no-unwind from LowIR bodies (fixpoint over the unit's call graph) so EH
  shapes and object bytes are unchanged for pa29-36.
- `trivial_lifecycle=yes` on implicit lifecycle helpers whose lowered
  complete-object bodies do no work (sema-owned fact).
- Implicit assignment operators lower memberwise in separate-compilation
  mode with the reference low-name spelling (`Class__operator___ovN`).
- `cppgm++ -c` sniffs LowIR input (leading `declare`/`global`/`function`/
  `alias` line) and skips the frontend; both compile paths run
  `parse -> validate -> optimize(level) -> MIR -> object`.

## Validation

- Fast loop: `make -C pa37 check TEST=tests/o1/<case>.t` per family;
  `make test-report ACTIVE_TEST_REPORT_PAS='pa37'` for the assignment.
- After each driver/toolchain change: `make test-report-through-pa37` (the
  required gate), watching pa13/pa14/pa28-36 for regressions.
- `perl scripts/cppgm_file_audit.pl --stage pa37 --paths dev/src` before
  committing; keep new files under the 1500/120 line limits.
