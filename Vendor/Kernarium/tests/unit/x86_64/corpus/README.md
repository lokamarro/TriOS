# x86-64 instruction unit corpus

`x86_64_v3.pack` is the build-generated, self-contained instruction input for
the ordinary `x86-64-instruction-corpus` CTest. It contains compact
per-instruction traces derived from the pinned K x86-64 semantics tests,
complementary handwritten FEX assembly programs, and the repository's own
cases in `cases/`. The `uml_x86_64_instruction_corpus` target generates it in
this directory's build-tree counterpart from the checked-in `source/` tables
with Python's standard library and no upstream checkout, assembler, network,
Git history, or reference hardware; the accepted provenance hash must match.
The pack is generated, never committed. The
[source contract](../../../../tools/instruction-corpus/README.md) owns the
table format.

The same pack also runs through the macOS-only x86-64 JIT, as
`x86-64-jit-instruction-corpus`, `x86-64-jit-code-buffer-growth` and
`x86-64-jit-emitted-registers`, against the classed exclusion list
`../jit_corpus_exclusions.tsv`, and through the tiered engine as
`x86-64-tier-switch-corpus`, against `../tier_switch_exclusions.tsv`.

## Profile and exactness

- The profile is portable x86-64-v3 userspace plus the AES, PCLMUL, XSAVE,
  FSGSBASE, ADX and AVX-VNNI extensions already owned by the product contract.
  Intel-only facilities, privileged instructions, obsolete vendor extensions
  and other post-AVX2 extensions are outside it. `excluded.tsv` records every
  producer input the profile omits, upstream rejects, FEX's own emulation
  replaces where x86 differs, or that is invalid as x86-64 or an exact
  duplicate. An included illegal instruction is always a test failure.
- `manifest.tsv` binds every packed record to its origin, input hashes,
  semantic forms and case count; `coverage.tsv` is the merged form inventory;
  `provenance.json` binds the pack to the pinned producer commits and Intel
  SDM revision. The upstream licence notices are retained alongside the source
  tables.
- FEX and native-Intel records are bit-exact. K-only NaN payload and sign
  choices are compared by IEEE NaN class only for the explicitly classified
  floating-point programs where Intel permits several quiet-NaN results. K
  computes RCP and RSQRT as exact quotients; where one strays from the native
  estimate by more than the SDM's estimate error, the native value replaces
  it.
- x87 is emulated only at full 80-bit precision. The producer drops FEX's
  `FEX_X87REDUCEDPRECISION` request, which only FEX's own translator honours,
  and keeps those programs' expected registers, which FEX's x86-64 host runner
  checks natively; the test runner rejects a case that still carries the
  request.

## Repository cases

x86 instruction correctness cases that neither upstream covers belong in
`cases/`, not in new focused tests; a focused test stays only where it needs
an engine hook the runner cannot give a guest program, such as a second
executor or engine-internal state. Each case is a GNU-syntax `.s` file whose
`_start` is linked at `0x10000` and ends at a `HLT`. One `/* CONFIG ... */`
block holds FEX's `MemoryRegions` (page-aligned address to size) and `RegData`
(RIP, the GPRs, and `XMM0`-`XMM15` as two or four 64-bit lanes) with the
meaning FEX gives them. A case starts from zeroed registers with IF set and may
rely on no mapping but its code and its regions. The only system call a case
may make is `time(NULL)` (RAX 201, RDI 0), which the runner answers with 1 and
the native run with the time, so a case that makes it leaves RAX undeclared.

A case's expectation has the authority of the other origins: the producer
accepts it only when a native Intel run of the case, from the same state to its
`HLT`, ends with exactly the declared values. The runner dispatches these cases
through the executor's program cache on the product's code-page model, so a
case can also hold the engine to its code-publication contract. Run one case
with `uml_x86_64_fex_asm_runner --repository BINARY CONFIG`.

## Regeneration

Regenerate only from verified producer inputs. The K semantics source tracks
paths that differ only by case, so its root must be case-sensitive, and
`prepare.sh` refuses any other:

```sh
dependencies/prepare.sh --product x86-conformance \
  --root "$CASE_SENSITIVE_DEPENDENCY_ROOT"
python3 -B tools/fex-tests/build-asm-corpus.py \
  "$CASE_SENSITIVE_DEPENDENCY_ROOT/fex" .build/fex-tests/corpus-x86-64 \
  --mode x86-64
python3 -B tools/x86-tests/build-unit-corpus.py \
  --k-source "$CASE_SENSITIVE_DEPENDENCY_ROOT/x86-k-semantics" \
  --native-request .build/x86-tests/native-k-request.bin \
  --repository-request .build/x86-tests/repository-request.bin
```

Build [`native-k-trace.cpp`](../../../../tools/x86-tests/native-k-trace.cpp)
as its header says and run it on an Intel Linux host once with each request on
standard input, saving its standard output as `native-k-response.bin` and
`repository-response.bin`. A new or edited repository case needs only a fresh
repository response. Complete the pack locally with:

```sh
python3 -B tools/x86-tests/build-unit-corpus.py \
  --k-source "$CASE_SENSITIVE_DEPENDENCY_ROOT/x86-k-semantics" \
  --fex-corpus .build/fex-tests/corpus-x86-64 \
  --native-response .build/x86-tests/native-k-response.bin \
  --repository-response .build/x86-tests/repository-response.bin
```

Producer output defaults to `.build/corpora/x86_64`. Add `--check` for a
byte-for-byte freshness check. Promote the accepted output with the source
contract's import command and `--kind x86`, committing its source tables and
metadata rather than the generated pack.
