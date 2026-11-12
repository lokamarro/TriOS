# arm64 gadget engine

The engine executes A64, A32 and T32 guest code as data-only streams of
pointers to handwritten gadgets. It never allocates or publishes executable
memory. The software TLB, the guest CPU state and the AArch32 decoder are
shared with the [JIT](../jit/README.md) and are not the engine's own. The
region's placement, exclusives, alignment, the SME profile and the AArch32
profile are cross-engine contracts in
[`docs/architecture.md`](../../../../docs/architecture.md). This file records
the contracts that are not obvious from the code; the design credit is
[`NOTICE.md`](NOTICE.md).

## Ownership

| Path | Role |
| --- | --- |
| `engine.S`, `engine/*.inc` | The gadgets. `engine/generate_streaming.py` and `engine/generate_a32.py` write the streaming-mode and AArch32 gadgets at build time. |
| `program_builder*.cpp`, `gadget_*selection*` | Turn decoded A64 instructions into streams. |
| `a32/` | The same for AArch32 ([`a32_builder.hpp`](a32/a32_builder.hpp)). |
| `executor.cpp`, `program_execution.cpp` | The translation cache and the run loop. |
| `sme_helpers.cpp`, `software_*_helpers.cpp` | Helpers the gadgets call; a call leaves streaming mode. |

The target is `uml_arm64_gadgets`. A retired entry stays in the bank
unselected, because removing it would move every later entry of the pinned
region. `uml_arm64_gadget_enter` saves the callee-saved registers in the frame
that [`gadget_host_frame.inc`](../../gadget_host_frame.inc) lays out for both
gadget engines, and `uml_arm64_gadget_leave` restores them and returns the
event in `w0` and the retired count in `x1`.

## Stream encodings

An arm64 gadget stream carries no PC word for a memory transfer. The operand of
every load, store, pair, atomic, exclusive, ordered and SIMD structure form
ends in the instruction's position in its block, in its top byte, so a block
holds at most 256 instructions. A fault reports the position shifted left by
two with bit 0 set, which no instruction address and no translation key has,
and the boundary turns it into the PC from the block holding the faulting
stream (`resolve_fault_position` for A64; a T32 block finds the instruction in
its halfwords). Only MOPS and a literal load whose target does not fit 48 bits
keep a PC word. `arm64-gadget-memory-fault-position` faults one instruction of
each family at several positions, directly and through a link. The packed
operand forms are pinned by `arm64-gadget-packed-immediate` and
`arm64-gadget-test-bit-branch`.

## Return ring and target table

A cached return consults a private per-carrier ring of return records that
linked calls push, and a cached indirect branch the address space's
direct-mapped target table that every miss exit trains, only after the site's
cells miss, so a stale or wrong record can only fall back to the ordinary miss
exit. The executor empties the ring on every address-space switch and after a
link sweep. The table belongs to the address space's link state and is retired
with it, so it never names another address space's code.

## Software TLB

The TLB's geometry, and why a transfer probes by the page of its final byte so
that a crossing can only miss, are in `darwin/memory/tlb.hpp`;
`arm64-tlb-probe-set` checks every inline probe of both engines against the
entry the C++ fill path placed. The fill path reads the entered generation's
fill index (`darwin/memory/mmu/fill_index.hpp`), the only record of what the
generation maps, without a lock, because whatever removes or changes a mapping
quiesces execution first.

With top-byte ignore on, the transfer, pair, structure, atomic,
compare-and-swap, ordered, RCpc and exclusive gadgets strip the tag with a
flag-guarded `ubfx` (`tbi_strip` in `engine/core_integer.inc`) after they form
the address and before they check alignment or probe. The TLB therefore holds
each page under its untagged key, and a misaligned or faulting access reports
the untagged address; a form that did not strip would evict, on every access,
the entry a plain access to the same page had just filled. An exclusive records
the host address after the strip, so its monitor never sees a tag. `arm64-tlb`
and `arm64-tbi-memory` (in the [instruction
corpus](../../../../tests/unit/arm64/corpus/README.md)) pin this.

## Streaming mode

Both engines run a guest in streaming mode on the host's own, and these rules
hold for the [JIT](../jit/README.md#streaming-mode) too.

- A translation belongs to one SVCR mode, PSTATE.SM and PSTATE.ZA, and an SVCR
  write ends its block. A program translated with SM = 1 runs with the host in
  streaming mode and, when ZA = 1, with guest ZA, and ZT0 under SME2, resident
  in the host's. Each SVCR mode has its own program cache and links.
- No C++ runs in streaming mode: a helper call leaves it, spilling that state.
  A streaming program may select only the entries `engine/sme.inc` lists, and
  `arm64-gadget-streaming-safety` fails on an AdvSIMD word executed in
  streaming mode, a helper called there, or an indirect call.
- XNU resets FPSR on every kernel entry while a thread is in streaming mode, so
  a streaming program keeps the guest's FPSR in `CpuState` and never runs with
  the native FP environment active: only the guest's FPCR, with its trap
  enables clear, is the host's while it runs.
- A streaming program runs every register-only SVE, SVE2 and SME form of the
  SME1 profile, every FP&SIMD data-processing class legal in streaming mode,
  and every SVE and SME1 load and store and SIMD&FP load and store legal there.
  SVE prefetches are NOPs. AdvSIMD, a word outside the profile and an SME2 form,
  which the guest is not told of, are the illegal-instruction exit.
- A memory access the native instruction cannot take goes to a helper that does
  the whole access on the register file, translating every page before it moves
  a byte, and reports a fault as M4 hardware does (`sme_helpers.cpp`; the JIT's
  is `jit/jit_semantic_sme.cpp`).

## AArch32

`a32/` builds the streams, and `run_translated_aarch32_blocks` in
`cpu/arm64/engine_execution.hpp` is the loop an engine's `run_aarch32` takes.
The gadgets come from the decoder's table by `engine/generate_a32.py` and the
hand-written `engine/a32.inc`, in a bank, a cold section and a veneer island of
their own after the SME part, so no A64 entry moves and the part holds no SVE or
SME word. The generators' docstrings (`engine/a32_*.py`) and `a32.inc` state
each family's operand layout and lowering.

- The exclusive and ordered transfers are the A64 part's gadgets, which read an
  address slot whole, so every slot a stream reads holds a zero-extended value.
- A block never ends inside an IT block: an entry with ITSTATE set, like a
  single step, runs one uncached instruction at a time until ITSTATE is zero.
- A word the decoder rejects and a coprocessor word the profile lacks end the
  block in the illegal-instruction exit whatever the condition.
- What the gadgets do not lower runs through the shared AArch32 semantic
  library (`routes_to_library` in `a32_builder.hpp` lists it) through the
  `a32_library` gadget, which commits the live FPSR to `CpuState` before the
  call and reinstalls it after. The library's unit-test variant counts its exits
  per form, and the unit tests that link it require that no lowered form
  reaches the library.
- VFP runs under the guest's FPCR in the native FP environment, and Advanced
  SIMD floating point under the standard FPSCR value. Each gadget starts with
  `a32_fp_standard` or `a32_fp_guest` (`engine/a32.inc`), which write the host
  FPCR only when `CpuState::a32_fp_standard` says the native environment holds
  the other one; the JIT follows the same rule.
- The engine decodes in the profile its host gives it
  (`a32::decode_profile`): the extensions the host has the A64 feature for are
  the ones the guest is shown, so a form of any other extension is UNDEFINED, as
  the HWCAPs say; the JIT derives the same profile.

## Build and test

```sh
cmake --workflow --preset check-unit
```

Focused tests: `ctest -R '^arm64-gadget-'` (stream encodings, streaming safety,
the AArch32 `-control`, `-forms` and `-corpus` tests, and
`arm64-gadget-region-layout`) and `ctest -R '^arm64-tlb'`. Instruction coverage
is in [`docs/a64-instruction-coverage.md`](../../../../docs/a64-instruction-coverage.md)
and [`docs/a32-instruction-coverage.md`](../../../../docs/a32-instruction-coverage.md);
the suites are in [`docs/testing.md`](../../../../docs/testing.md).
