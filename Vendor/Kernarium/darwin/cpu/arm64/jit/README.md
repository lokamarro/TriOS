# arm64 JIT engine

The JIT emits native blocks for A64, A32 and T32 guest code into its executable
arena. It runs on macOS only. The gadget engine includes none of it. Exclusives,
alignment, the SME profile, the AArch32 profile and the semantic slow path's
role are cross-engine contracts in
[`docs/architecture.md`](../../../../docs/architecture.md). This file records
the lowering contracts that are not obvious from the code; the
[gadget engine](../gadgets/README.md) states the rules both engines share.

## Ownership

| Path | Role |
| --- | --- |
| `jit_arena.cpp` | The executable arena. It reaches executable memory only through `cpu/jit_memory/executable_memory.cpp`. |
| `jit_compiler*.cpp` | Lower A64. |
| `a32/` | Lowers A32 and T32 beside the A64 compiler and without changing it. |
| `jit_executor*.cpp`, `jit_linking.*`, `jit_shared_translations.*` | The translation cache, the links and the shared translation tier. |
| `jit_semantic_*.cpp` | The semantic slow path for what the compiler does not lower. |

The target is `uml_arm64_jit`.

## Cold code

The JIT compiles a block the first time an address space runs it, and no
translation serves another process: the private cache is keyed by the address
space, the shared tier opens only while two executors live and is keyed by
guest PC, and the guest's ASLR moves every image anyway. Each exec'd process
therefore compiles its whole startup path again, several times what it costs
the gadget engine, and the LTP runner scales its case bounds for it
([testing](../../../../docs/testing.md#ltp)). The JIT runs only on macOS, so the
cold path is not optimized
([history](../../../../docs/optimization-history.md#jit-cold-code-per-process)).

## Top-byte ignore

When the guest ignores the top byte of data addresses (`CpuState::tbi_enabled`,
which the host turns off with `--guest-no-top-byte-ignore`), a translation is
compiled for it and its key says so (`translation_mode_tbi`), so a block runs
only under the setting it was translated for. Every access, whatever register
is its base, then strips bits 63:56 of its final address with one `ubfx` before
the address is translated, and a translation compiled without it holds no check
at all. The TLB, a page fault and the fault address a signal reports therefore
see the untagged address, as the semantic path reports it. Exclusives, atomics
and ordered accesses strip before they translate and record the host address,
so the monitor guards the location and not the pointer.

## Streaming mode

The JIT follows the streaming-mode rules that the
[gadget engine](../gadgets/README.md#streaming-mode) states for both engines.
What is the JIT's own:

- Every translation, link and indirect site is keyed by the guest's SVCR mode as
  well as its PC, so a block runs only in the mode it was translated for; mode 0
  translations are unchanged. SM=0 translations run with the host's SME off and
  hold no SVE or SME instruction.
- An SM=1 translation keeps the guest's Z, P, ZA and ZT0 in the identically
  numbered host registers: its external prologue enters streaming mode and
  fills them from the register file, and every epilogue and host helper call
  first leaves streaming mode and spills them. Helpers are terminal, so no SM=1
  code runs after one, and a TLB miss takes a streaming bridge that leaves and
  re-enters around the walker. The transitions preserve NZCV, and SM=1 code
  emits no AdvSIMD instruction of its own.
- A load or store runs as the guest's own word when every byte it can span
  lies in one guest page the TLB holds; a crossing, a TLB miss, a failed
  translation or a misaligned SP base takes a terminal helper that does the
  whole access. SME2, every other SVE and SME word, and AdvSIMD and the
  FEAT_LSFE and FEAT_LSUI loads and stores in streaming mode are the
  illegal-instruction exit.

## AArch32

`A32Compiler` ([`a32_compiler.hpp`](a32/a32_compiler.hpp)) reuses the A64
compiler's register residency, flag machine, cold paths, direct and indirect
links, tracked stores and publication. The comments of `a32/` state each
family's lowering; these contracts span them.

- R0-R14 are resident host registers (`resident_gpr_hosts` in
  `jit_compiler_families.hpp`). A linked translation keeps them there from
  block to block: the registers are loaded before the block's internal entry,
  which only the executor reaches, a link leaves them in their hosts, and every
  return to the executor, a refused link included, writes all fifteen back.
  Link sequences therefore use x10 and x11, and no lowering may use a host
  register of a core register as scratch. An unlinked translation (the
  tracked-store variant, a single instruction) loads at first use and writes
  back at every exit.
- At the start of a block a load-exclusive, up to four register instructions and
  the store-exclusive on the same address become one host load-exclusive and
  store-exclusive, so the guest's monitor is the host's
  (`a32_compiler_exclusive.cpp` says what fuses and what it declines).
- What the lowering declines leaves the block through
  `uml_arm64_jit_a32_execute`, a terminal helper that runs the one instruction
  in the AArch32 semantic library over the TLB, with the library's fault
  ordering. An entry in the middle of an IT block and a single step run one
  instruction in the single-instruction arena, with the IT state the exit
  recorded.
- VFP and Advanced SIMD keep Q0-Q15 in the A64 compiler's vector residency, and
  the switches between the guest's FPCR and the Standard FPSCR value follow the
  [gadget engine's rule](../gadgets/README.md#aarch32). The executor installs
  the guest's FPCR and FPSR for a block that holds a VFP instruction.
- The JIT presents the Armv8 extensions where the host has their A64 feature,
  decoding against the same profile as the gadget engine (`a32_decode_profile`),
  so a form of an extension the host lacks is UNDEFINED and never reaches the
  semantic library. It lowers the forms that are the host's own instruction
  (`a32_compiler_armv8.cpp`) and runs the rest in the semantic library.

## Build and test

```sh
cmake --workflow --preset check-unit
```

The JIT's tests run on macOS only: `ctest -R '^arm64-jit-'` (the AArch32 ones
are `arm64-jit-a32-*` and `arm64-jit-executor-paths`). Instruction coverage is
in [`docs/a64-instruction-coverage.md`](../../../../docs/a64-instruction-coverage.md)
and [`docs/a32-instruction-coverage.md`](../../../../docs/a32-instruction-coverage.md);
the suites are in [`docs/testing.md`](../../../../docs/testing.md).
