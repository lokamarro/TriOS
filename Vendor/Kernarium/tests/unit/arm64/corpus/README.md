# A64 instruction unit corpus

`a64_instruction_corpus.pack` is the build-generated, self-contained input of
the ordinary `arm64-instruction-corpus` CTest. Its runner,
[`../instruction_corpus_runner.cpp`](../instruction_corpus_runner.cpp),
executes every case through the production gadget engine and, when the host
implements the case's native features or the case is an invalid encoding,
through the JIT. A case that needs FEAT_LSE2 runs in neither lane on a host
without it, and a case that needs FEAT_AFP or FEAT_RPRES runs in neither lane
where the virtual CPU does not implement it. The runner needs no Python,
network, or file other than the pack.

The pack is generated, never committed. The checked-in `source/` tables hold
the programs and the golden expectations; the `uml_arm64_instruction_corpus`
target generates the pack in this directory's build-tree counterpart and
verifies its accepted hash. The
[source contract](../../../../tools/instruction-corpus/README.md) owns the
table format and the promotion of accepted producer output. Expected results
come from independent derivation from the architecture or from observation on
native hardware, never from an engine's output.

```sh
ctest --test-dir .build/presets/macos-unit -R '^arm64-instruction-corpus$' \
  --output-on-failure
```

`uml_arm64_instruction_corpus_tests PACK` takes record, case, engine and
feature filters (it prints its options when started without arguments).
[`docs/testing.md`](../../../../docs/testing.md#cpu-instruction-tests) lists
the CTests that gate the special records below.

## Files

- `manifest.tsv`: one row per record, with case counts and hashes.
- `coverage.tsv`: cases per `../a64_instruction_inventory.tsv` entry, with
  illegal-instruction cases counted separately.
- `provenance.json`: the producer commit, sampling limit and seed, per-step
  script hashes and counts, and the pack hash.
- `forced-cases.tsv`: producer checks that the class samples leave out.
- `absent-feature-failing.tsv`: cases the absent-feature rewrite drops because
  they fail once a removed feature's state is stripped.

## A case

A case is one engine-harness check: a program on the code page at
`0x4f000000`, the initial architectural state and data regions, and the
expected terminal event, retired-instruction count, fault address and access,
floating-point syndrome, architectural state, and data bytes. The code page
must be unchanged after every run. A case whose architecture permits several
results names a validator relation instead of an exact state.

The producer export classifies every case by check site, terminal event,
invalid-encoding flag, harness options, retirement and fault expectation,
validator relation, native requirements, entry offset, and each program word's
decode partition, allocation predicates and register-31 fields. Each class
keeps its first and last case and a fixed-seed uniform sample, at most eight,
and every case that gives an operand field a boundary value (0, 30, 31 for a
register field; 0, 1 for a bit; 0 and all ones for a wider field) that no
earlier case of the class gave it. A case is exported only after the
producer's interpreter, gadget and JIT lanes all pass it, and its registers
are minimized to those the program can name. Every record is for the 4 KiB
guest page.

A gadget or JIT case enters the engine as the runtime enters a task: the
initial vectors are deferred and `CpuState::vectors` holds nothing, so the
runner compares only what a context switch would save (the vectors if the
engine marked them written, FPCR and FPSR as the deferred image reports them).
A case that expects a vector write therefore fails for an engine that did not
load the vectors first.

### Validator relations

| Relation | Accepted result |
| --- | --- |
| `host-counter-frequency/v1` | expected state with the host's CNTFRQ_EL0 in each MRS CNTFRQ_EL0 destination |
| `ignore-host-counter-register/v1` | expected state except system-register read destinations |
| `permitted-exclusive-store-failure/v1` | expected state except the opaque exclusive-monitor value and version |

Relations derive their operands from the program, so a new relation or a new
use of an existing one requires a runner change.

## Pack format

All integers are little-endian. The file is `UMLA64C\0`, u32 version 2, u32 0,
and u64 body length, followed by the zlib-compressed body:

```text
body     = meta:str32  tokens:u16*str16  native:u16*(u8*u16)
           validators:u16*str16  blobs:u32*(u32 size, bytes)  records:u32*record
record   = name:str16 producer:str16 site:str16 profiles:u8 options:u16 svl:u16
           programs:u32*(entry:u16, u16*(offset:u16, word:u32))  cases:u32*case
case     = program:u32 name:str16 ordinal_16k:u32 ordinal_4k:u32 event:u8
           flags:u8 retire [fault] [syndrome] native:u16 validator:u16
           initial:state expected:state regions:u8*region
retire   = 0 exact:u32 | 1 minimum:u32 step:u32 maximum:u32 | 2
fault    = address:u64 access:u8        present when flags bit 1 is set
syndrome = u32                          present for floating-point-trap
state    = u32 mask, u64 per set bit (bit 31 is SP)
           u32 mask, 16 bytes per set vector
           [sme]                                present when svl is not 0
           u8 count, (field id:u8, value) per field
sme      = u32 mask, svl bytes per set Z register
           u16 mask, svl/8 bytes per set P register
           svl/8-byte mask, svl bytes per set ZA row (horizontal vector)
           u8 0 | u8 1, 64 bytes                ZT0
region   = address:u64 size:u32 flags:u8 blob:u32
           u16*(offset:u32, length:u16, bytes)   expected bytes
           u16 0  u16 0                          retired tag lists
```

- `str16` and `str32` are length-prefixed UTF-8; `meta` is JSON. Native sets
  index the stable `arm_feature_tokens` table of
  `darwin/platform/host_capabilities.hpp`.
- The initial state is encoded against a base state and the expected state
  against the initial one. The state field ids, the option bits and event
  order, the region flags, and the base register, vector and SME fills are
  defined by
  [`build-unit-corpus.py`](../../../../tools/arm64-tests/build-unit-corpus.py)
  (`STATE_FIELDS`, `OPTION_NAMES`, `EVENTS`, `fill_*`), which the runner
  mirrors.
- `profiles` bit 1 is the 4 KiB page; bit 0 (the retired 16 KiB profile) is
  rejected by the builder and the runner, and `ordinal_16k` is zero. Ordinals
  are producer check ordinals, zero where the export did not hold the case.
- Option bit 10 (`without_afp`) and bit 11 (`without_sme`) translate the
  record for a virtual CPU without FEAT_AFP or FEAT_SME on every host.
- Slots of removed features (option bits 0, 1 and 12, event code 2, the field
  ids `build-unit-corpus.py` lists as retired, and non-empty tag lists) stay
  reserved; the runner rejects a pack that uses them.
- Validator `0xffff` means exact comparison; retirement maximum `0xffffffff`
  is unbounded.
- The case flags are the invalid-encoding bit, the fault-compared bit, and the
  minimization level (0 full, 1 narrow, 2 broad) in bits 2-3; see
  [Invalid-encoding bits](#invalid-encoding-bits).
- `build-unit-corpus.py` reads pack versions 1 and 2 (version 1 has no SVLs)
  and writes version 2.

A record's `svl` is the streaming vector length in bytes its cases' SME state
is sized for, or 0 for a record without SME state. Only a record with an SVL
holds Z, P, ZA, ZT0, `svcr` or `tpidr2`, and the runner runs its cases only
where the virtual CPU implements FEAT_SME with that SVL. Z and P exist only
while PSTATE.SM is set (the V registers are then the low 16 bytes of the Z
registers), and ZA and ZT0 only while PSTATE.ZA is set.

## Regeneration and new cases

The exporter, its driver (`tools/arm64-tests/export-unit-corpus.py`), the
producer unit tests and the harness interpreter lane exist only in the producer
commit that `provenance.json` names as `producer_commit`. Ordinary source-table
materialization does not need that commit. To add a case, check out the
producer commit, copy the current `forced-cases.tsv` into it, add a check in a
producer unit test or force an existing one, and build and export both page
profiles there. Then write the exports as a dump with this checkout's
`build-unit-corpus.py --export-16k DIR --export-4k DIR --include
forced-cases.tsv --producer-commit COMMIT --text corpus.jsonl`.

`forced-cases.tsv` forces producer checks that reach something no sampled case
reaches; the comment before each directive names it. Its `case` directives
name producer check ordinals, so a producer change that renumbers checks must
re-pin them; the export summary reports a directive whose check is missing or
excluded.

Each later step reads the previous step's dump, replaces its own records, and
appends them, so the steps run in this order, followed by one `--repack` and
`build-unit-corpus.py --check`:

1. [LSE2 alignment](#lse2-alignment-cases),
2. [absent features](#absent-feature-cases),
3. [native SME](#native-sme-cases),
4. [native floating-point trap](#native-floating-point-trap-cases),
5. [SM = 0 family](#sm--0-family-cases),
6. [top-byte-ignore memory](#top-byte-ignore-memory-cases),
7. [16 KiB retirement](#16-kib-retirement), and
8. [invalid-encoding bits](#invalid-encoding-bits), always last.

A hardware step's `--carry PACK` copies the pack's records for a request that
is unchanged, so a regeneration that changes no request needs no host. Each
step's script docstring owns its usage, its cases and its provenance keys.

`--check` verifies that the metadata and canonical encoding match the pack,
that every inventory entry has a case or an illegal-instruction case, and that
every row of
[`../a64_instruction_evidence_bindings.tsv`](../a64_instruction_evidence_bindings.tsv)
names, by producer and case name, a pack case whose entry word is its witness,
or `-` for both columns. A regeneration that drops a binding's case names
another case that executes the witness, moves the witness to the entry word of
a kept case that maps to the same allocation row (only `binding_encoding`
changes), or sets `ctest` and `case` to `-`. Promote the accepted pack with the
source contract's import command and carry the source tables, metadata and
`forced-cases.tsv` back as one change.

## LSE2 alignment cases

The producer checks predate the FEAT_LSE2 alignment rule and expect an
alignment fault from a misaligned LSE atomic or exclusive access inside one
16-byte block, and the producer's interpreter lane does not implement the rule.
[`lse2-alignment-corpus.py`](../../../../tools/arm64-tests/lse2-alignment-corpus.py)
drops those cases and appends the hand-authored `arm64-lse2-alignment`
records, which follow the architecture and match native LSE2 hardware.

## Absent-feature cases

The virtual CPU implements none of FEAT_PAuth, FEAT_MTE, FEAT_GCS, FEAT_CPA
and the other features that
[`absent-feature-corpus.py`](../../../../tools/arm64-tests/absent-feature-corpus.py)
lists: their non-hint encodings are UNDEFINED, their hint-space encodings are
NOPs, and FPCR.EBF is RES0 (the pack encoder refuses a state that sets it).
Every SVE encoding outside streaming mode is UNDEFINED except the
[SM = 0 family](#sm--0-family-cases). The script drops the producer cases that
exercise a removed feature, strips its state and options from the rest, and
appends the `arm64-absent-<feature>` records, which expect every encoding class
to be undefined and the hints to be NOPs. FEAT_SME is two-sided, like
FEAT_AFP: the virtual CPU implements the SME profile only where the host does.
FEAT_SVE is absent on every host, so no case holds non-streaming SVE state.
The witness of every row of the generated SVE/SME encoding table
([`sme_encodings.hpp`](../../../../darwin/cpu/arm64/detail/sme_encodings.hpp))
is pinned UNDEFINED at PSTATE.SM = 0 in the record whose option makes it so.

The virtual CPU implements FEAT_AFP exactly where `implements_afp()` in
[`floating_controls.hpp`](../../../../darwin/cpu/arm64/floating_controls.hpp)
says, and FEAT_RPRES where `implements_rpres()` says. A case whose FPCR sets
FIZ, AH or NEP carries a native `afp` requirement (the pack encoder refuses a
state that sets them without it), and a case that pins those controls RES0
lives in a `without_afp` record. No case pins AH tininess for FADD, FMUL, FDIV,
FSQRT, FMADD or vector FMLA: on a FEAT_AFP host both engines run them natively
under the guest FPCR, so a native oracle run on such a host covers it.

## Native SME cases

[`sme-native-corpus.py`](../../../../tools/arm64-tests/sme-native-corpus.py)
authors the `arm64-sme-native` records from the generated SVE/SME encoding
table, and
[`sme_native_oracle.cpp`](../../../../tools/arm64-tests/sme_native_oracle.cpp)
observes their outcomes on an SME host. Every native record has `svl` 64, the
only streaming vector length with native expectations. Where the encoding
table decides a case's legality, the script refuses a hardware response that
disagrees, so a disagreement is a finding about the table, never an
expectation. Regenerate on an SME host, twice from clean
directories, and require identical responses.

## Native floating-point trap cases

Apple's cores trap enabled floating-point exceptions, so the hardware decides
what a trapped FP&SIMD instruction does.
[`floating-trap-native-corpus.py`](../../../../tools/arm64-tests/floating-trap-native-corpus.py)
replaces the outcomes of the register-only `arm64-floating-trap-*` cases with
the host's, appends the authored `arm64-floating-trap-native` records, and
refuses a response whose trap reports more than one cause or changes state at a
program's entry. Any arm64 Mac regenerates the records, twice from clean
directories; the cases that need FEAT_AFP or FEAT_BF16 form a second request
for a host with both.

## SM = 0 family cases

The virtual CPU, with FEAT_SME and never FEAT_SVE, executes the vector-length
family (CNT[BHWD], RDVL, ADDVL, ADDPL, scalar INC and DEC, and LDR and STR of Z
and P with a scalar base) at PSTATE.SM = 0 for a constant vector length of 128
bits. This is a deliberate departure from the architecture, which makes them
UNDEFINED there without FEAT_SVE, and an M4 traps them, so no hardware result
exists: the expectations are the instructions' pseudocode with the constant
length, written independently of the engines (see
[`sm0_vector_length.hpp`](../../../../darwin/cpu/arm64/detail/sm0_vector_length.hpp)
and [`docs/architecture.md`](../../../../docs/architecture.md)).
[`sm0-vl-corpus.py`](../../../../tools/arm64-tests/sm0-vl-corpus.py) writes the
one `arm64-sm0-vl` record, with `svl` 64 and no hardware requirement, so
`arm64-instruction-corpus-sm0-vl-profile` runs it with the SME profile assumed
on any host. `arm64-absent-sve` under `without_sme` pins the same words
UNDEFINED where SME is not implemented.

## Top-byte-ignore memory cases

With top-byte ignore (TBI) a data address's top byte is not part of the
address: a fault reports the untagged address, the base register keeps its tag
(also across a writeback), and the exclusive monitor does not see the tag.
Android runs every process this way, and the virtual CPU does unless the host
passes `--guest-no-top-byte-ignore` (`CpuState::tbi_enabled`).
[`tbi-memory-corpus.py`](../../../../tools/arm64-tests/tbi-memory-corpus.py)
authors the `arm64-tbi-memory` record (atomic, ordered and exclusive forms,
expectations from the architecture's pseudocode) and the
`arm64-tbi-equivalence` records, each an existing corpus case rerun with its
base register tagged. The `arm64-tlb` unit test and the `guest_tagged_*_loop.S`
probes ([performance tools](../../../perf/README.md#focused-assembly-probes))
own which TLB key an engine fills.

## 16 KiB retirement

The guest has 4 KiB pages only, but the producer export ran both page
profiles. [`retire-16k-corpus.py`](../../../../tools/arm64-tests/retire-16k-corpus.py)
rewrites the dump: a record for both profiles becomes a record for 4 KiB, a
record for 16 KiB alone is deleted, and a deleted case whose entry word no
4 KiB case executes is relocated into the 4 KiB code page, so the pack's
encodings do not shrink. The step is not idempotent: a dump taken from the
pack itself is already retired, so skip it there and go straight to the
invalid-encoding step.

## Invalid-encoding bits

A case's invalid-encoding bit is set when no virtual-CPU configuration decodes
the word as an instruction: an unallocated word, a reserved or CONSTRAINED
UNPREDICTABLE operand value, a permanently undefined word, or an encoding of a
feature the virtual CPU never implements. A clear bit on an
illegal-instruction case means a valid encoding that traps for its exception
level, mode or state, or because a feature that some virtual-CPU configuration
implements (SME, FEAT_AFP) is absent from this one.
[`invalid-encoding-corpus.py`](../../../../tools/arm64-tests/invalid-encoding-corpus.py)
owns the bit and is the last step of the chain; it sets it for each
illegal-instruction case that retires nothing from the answer of
[`invalid_encoding.py`](../../../../tools/arm64-tests/invalid_encoding.py), and
a word the checked-in tables do not settle keeps its producer's bit. The
`arm64-instruction-corpus-invalid-encoding` tooling test requires the settled
cases to carry it, and its opt-in `-llvm` sibling checks every clear bit
against the pinned LLVM MC.
