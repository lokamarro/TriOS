# AArch32 instruction corpus

`a32_instruction_corpus.pack` is the build-generated, self-contained input of
the `arm64-aarch32-corpus` CTest. A case is one program of the AArch32 profile
(Armv8-A AArch32 at EL0, see
[`../../../../docs/a32-instruction-coverage.md`](../../../../docs/a32-instruction-coverage.md)),
the state it starts in, and what an oracle observed it do. The checked-in
`source/` tables hold the programs, the starting states and the observations;
CMake's `uml_arm64_aarch32_corpus` target generates the pack from them and
verifies the SHA-256 in `provenance.json`
([source contract](../../../../tools/instruction-corpus/README.md)). The pack
is generated, never committed.

The runner,
[`../aarch32_corpus_runner.cpp`](../aarch32_corpus_runner.cpp), needs only the
pack and proves each case consistent with the decoder and with itself. An
engine lane includes the reader and case layout,
[`../aarch32_corpus_pack.hpp`](../aarch32_corpus_pack.hpp), and the state
conversion,
[`../aarch32_corpus_cpu_state.hpp`](../aarch32_corpus_cpu_state.hpp), runs each
case and compares what the pack records; the inventory's `engine_status` moves
from `pending` to `verified` per entry. A lane starts every case on a virtual
CPU that has run 64-bit code: the general-register slots that hold no AArch32
register keep values with both halves set, as an AArch64 task leaves x15-x30
and SP. A fault address wider than 32 bits, or a register slot left with its
upper half set, fails the case.

## A case

A program is A32 words or Thumb halfwords placed at `0x4f000000` plus the
case's origin, entered in the instruction set the case names, in the memory
map of [`a32_oracle.cpp`](../../../../tools/arm64-tests/a32_oracle.cpp) (code,
exit stubs, data and read-only data; everything else is unmapped). Every
register is the test's: the oracle loads r0-r14, APSR, FPSCR, TPIDRURO,
TPIDRURW and D0-D31, runs the program, and records the same state at the exit
stub or at the fault, plus the data bytes the program changed. Starting states
are the named recipes of `source/states.jsonl`; a case names a recipe and the
registers it changes.

Each `source/<suite>-<oracle>.jsonl` starts with a header (`record`, `oracle`,
`defaults`, `environment`) and then one object per case:

| Key | Meaning |
| --- | --- |
| `n`, `f` | case name and the inventory encoding of the instruction under test |
| `i`, `p` | `A` or `T`, and the program in hexadecimal (words, or halfwords) |
| `t` | ordinal of the instruction under test |
| `o` | byte offset of the program from `0x4f000000` (default 0) |
| `s`, `si` | starting-state recipe and changes to it |
| `d`, `dp` | data fill (default `hash32`) and data patches `[offset, bytes]` |
| `x` | registers that differ from the start when the program ends or faults |
| `e`, `fi`, `fa` | event (a signal number; 255 for a system call), halfword index of the instruction it was raised at, and the fault address |
| `k` | instructions retired before the event or in all (default 1) |
| `m` | data bytes the program changed, `[offset, bytes]` |
| `fl` | `s` the program is straight-line, `x` it ended in Thumb state |

An event stops the program with the state at the fault; a SIGILL's address is
the instruction's. A program with branches is not straight-line: its retired
count is the plan's, which the producer checks against the oracle's final
state. The pack's byte layout is
[`tools/instruction-corpus/aarch32.py`](../../../../tools/instruction-corpus/aarch32.py).

## Oracles and suites

A record holds one suite observed by one oracle and is named
`<suite>-<oracle>`.

- `qemu`: `qemu-arm -cpu max` on the build host, run twice per case; both runs
  must agree.
- `profile`: no oracle ran it; the event the profile defines.
- `pi4` and `pi5`: a Raspberry Pi 4 (Cortex-A72) or 5 (Cortex-A76) running the
  oracle natively under its 64-bit Linux, twice per case.

The planner modules `tools/arm64-tests/a32_corpus_*.py` define the suites,
grouped by level (base A32 and Thumb-2, boundary and sequence suites, `simd`,
and the Armv8 additions of `complete`); their docstrings list each suite's
cases. Three suites are recorded only from hardware, as `pi4` records:
`unaligned`, `positions-unaligned` and `fpscr-reserved`. The producer refuses
to record them as `qemu`, and nothing derives them from rules.

The oracle's store-exclusive always succeeds when its load-exclusive ran
first, and a case is one attempt. An engine whose exclusives are the host's can
fail one without cause, as the architecture allows, so a lane runs a failing
store-exclusive case again, up to 16 attempts in all, when the only difference
is a status of 1 where the pack has 0 and the store that did not happen.

What the oracle's answers rest on:

- Only the signal number, the fault address and the registers and memory
  after the program are golden; `si_code` differs between qemu and Linux.
- A misaligned multi-word access (LDM, STM, LDRD, STRD, PUSH, POP) is an
  alignment fault, SIGBUS at its first address: the profile has no fixup, and
  neither has the `pi4` oracle's kernel.
- FPSCR exception trap enables read as zero and ignore writes, as the profile
  does; Stride and Len belong to `fpscr-reserved` alone. FZ16 is FEAT_FP16's
  and holds wherever that is, so the `fpscr`, `armv8-fz16` and
  `armv8-convert16` suites write it and every other suite starts with it
  clear.
- CNTFRQ and CNTVCT depend on the host and are not in the corpus.

## Hardware cross-check

`compare` checks every `qemu` record against another oracle's observations of
the same cases. It agrees on every case except the ones whose instruction
needs a feature the Cortex-A72 lacks (left out with `--absent`) and the
differences that [`known-differences.tsv`](known-differences.tsv) lists with
their reasons. There the architecture, Linux or the A72's missing features
leave the answer open, and the corpus keeps qemu-arm's, which is the profile's.
`observe` runs each case twice, and the two runs must agree, except in the
fields `known-differences.tsv` lists for the oracle and the case; any other
difference fails it and names the cases.

## Regeneration

```sh
python3 tools/arm64-tests/a32-corpus.py observe --archive ISA_AArch32_xml_A_profile-2026-06.tar.gz \
  --suite alu --host HOST --runner 'qemu-arm -cpu max'
python3 tools/arm64-tests/a32-corpus.py finish
```

`observe` plans a suite from the decoder's table (it needs the pinned Arm XML
archive, which the planner only reads), builds the freestanding armhf oracle
with the pinned clang and lld, runs it in a private directory on the host and
removes the directory. `finish` regenerates `provenance.json`, `manifest.tsv`,
`coverage.tsv` (cases per inventory entry) and `excluded.tsv` (entries with no
case). Commit the source tables and these files, never the pack.

`check --archive ...` plans every suite again without an oracle and requires
each source table to hold exactly the planned cases, in order (the opt-in
`arm64-a32-corpus-plan-freshness` test runs it); a change to the forms or the
planner that moves a case fails it until the suite is observed again. Hardware
records use the same producer without `--runner`, so the harness runs natively
on an aarch64 Linux host whose cores run AArch32 at EL0 (a Raspberry Pi 4 or 5
with a 64-bit kernel). `a32-corpus.py` documents `compare` and the other
options.
