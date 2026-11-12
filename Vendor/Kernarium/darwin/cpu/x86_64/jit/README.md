# x86-64 JIT engine

`fex_engine.cpp` runs x86-64 userspace, long64 and compat32, through FEXCore's
x86 frontend, IR passes and arm64 backend, embedded so that guest memory stays
behind the static MMU. It is a macOS product only;
[architecture](../../../../docs/architecture.md#cpu-execution-and-scheduling)
records how the build, the audit and the iOS packaging prove it. FEXCore comes
from the pinned dependency and its maintained patch series
(`dependencies/patches/fex/`). This file records the contracts that are not
obvious from the code.

## Ownership

| Path | Role |
| --- | --- |
| `fex_engine.cpp`, `fex_engine.hpp` | The engine: FEXCore contexts per address space, the software TLB miss path, exits and the embedder hooks. |
| `../fp_model.*` | The SIMD floating-point model the engine's cold checks call (shared with the gadget engine). |
| `darwin/runtime/x86_jit_runtime.cpp` | The runtime seam; the only code outside this directory that includes the engine. |

The target is `uml_x86_64_jit` (`UML_BUILD_X86_64_JIT`). The guest CPU state,
decoder, CPUID, software TLB entry, code-page registry, split-operand lock,
segment model and SIMD floating-point model are `uml_x86_64_target` in the
parent directory, shared with the [gadget engine](../gadgets/README.md). This
target links that and the executable-memory owner, never the gadget engine.
FEXCore allocates and publishes code only through the owner's hooks
(`darwin/cpu/jit_memory/executable_memory.cpp`), and its call-return stack,
which generated code addresses without a bounds check, sits in the owner's
guarded data reservation. A dispatch starts at most 64 Ki instructions, which
keeps unbalanced calls and returns inside that stack.

## Selection

On the x86-64 product, `--execution-engine jit` selects the engine and the
default is the gadget engine, by name. Selecting the JIT fails where the build
or host cannot run it (JIT memory with a write window, FEAT_LSE and FEAT_LSE2).
The arm64 product's x86 test guest builds the engine for its unit tests only
and runs x86 code on the gadget engine.

## Tiered execution

`--jit-tier=N` starts each x86-64 address space on the gadget engine and gives
it to the JIT once the gadgets have retired N instructions in it, so a process
that exits first never pays the JIT's compile cost. Zero, the default, runs the
JIT from the first instruction. The count is per address space and the JIT
never hands a space back.

Neither engine keeps state across a dispatch, so the engine can change between
dispatches, provided no gadget dispatch is still running in the address space.
The handover drains: once a space crosses its threshold no dispatch of it
starts on the gadgets, each one running leaves, the last opens the JIT, and
dispatches that begin meanwhile wait ([`x86_tier.hpp`](../../../runtime/x86_tier.hpp)).
Every exec'd process compiles its startup path from scratch on the JIT, so the
LTP runner scales its case bounds ([testing](../../../../docs/testing.md#ltp)).

`x86-64-tier-switch-corpus` and `x86-64-tier-switch-retirement` hold the
handover to the engines' own results.
[`tier_switch_exclusions.tsv`](../../../../tests/unit/x86_64/tier_switch_exclusions.tsv)
lists the one record a handover changes. The x86 guest runners take
`UML_TEST_JIT_TIER=N` to run on the tiered JIT unchanged.

## Address spaces and memory

- One engine belongs to one vCPU thread. It keeps a FEXCore context per
  address-space generation, execution mode, store order and floating-point
  mode, at most eight, because FEXCore's block caches are keyed by guest RIP
  alone. The runtime tells the engine which generations still exist
  (`FexEngineOptions::generation_live`; a generation is never reused). The
  space of a gone one becomes the next address space's, or is released before
  the least recently used when the engine is full.
  `x86-64-jit-space-recycling` holds the process's mappings flat across engines
  made and destroyed.
- Segmentation belongs to the model both engines share
  ([`segments.hpp`](../segments.hpp)). FEXCore ends the dispatch unretired
  before an instruction that needs it, and the engine runs the instruction
  through the model. A compat32 address wraps at 4 GiB, and SYSCALL and
  SYSENTER raise `#UD` there; compat32 code makes system calls with INT 0x80.
- FEXCore's generated code reaches guest memory through an inline software TLB.
  A miss calls the engine, which fills the entry from the static MMU or ends
  the dispatch with the guest's fault, and the miss path preserves every
  register the access sequence holds. An access that crosses a guest page, or
  an ordered access that crosses a 16-byte granule, completes through a bounce
  buffer in the address space's own fault order; a locked operand that crosses
  a granule runs under the process-wide split-operand lock, as in the gadget
  engine.
- The engine joins the code-page registry with a function that retires its own
  TLB entry for a page, and counts a page in before the decoder reads its
  bytes. Only a store to a page the registry holds publishes a code change,
  and each such store publishes once it has landed. A publishing store into the
  code of the unit running it stops that unit before it stores. The engine
  learns of the address space's changes as two histories, mapping and code
  (see [`uml_darwin_static_mmu.h`](../../../include/uml_darwin_static_mmu.h)); a
  reader further behind than the history reaches drops every translation of the
  space.
- The runtime hands an engine the histories at every entry, so a vCPU looks
  for code changes only between runs. CPUID and IRET are x86's serializing
  instructions in user code and each ends the run (`serialized`); the runtime
  then hands over the histories before the next instruction. SERIALIZE is not
  decoded and raises `#UD`, as on the gadget engine.
- Embedded FEXCore has no guard-page signal handler, so the maintained series
  bounds every write to a code buffer and restarts the compile with twice the
  space on an overrun (`code_buffer_restarts()`;
  `x86-64-jit-code-buffer-growth` makes every large unit grow). A space drops
  its translations without allocating, and a buffer grows only when the
  translations of one space fill it.

## State at every exit

A dispatch leaves the engine at a system call, a fault, a TLB miss the runtime
must resolve, an exhausted budget or a pending interrupt, often in the middle
of a block. At each exit the guest registers, RFLAGS, the vector registers and
the x87 stack and TOP are precise. A fault leaves the state as its instruction
found it, because the guest resolves the fault, a copy-on-write store among
them, and runs the instruction again; register allocation therefore never
commits a value to a guest register before a later access of the same
instruction that can fault. The exceptions probe's fault-state mode holds both
engines to that.

A dispatch reports as retired only the instructions that completed, as the
gadget engine does, and retires exactly one instruction, or one element of a
repeated string instruction, when RFLAGS.TF is set on entry. The `--retirement`
cases of the corpus runners hold both engines to the same counts. A signal
handler without SA_RESTORER returns to `DARWIN_UML_SIGRETURN_TRAMPOLINE`, which
no guest page backs on the x86-64 slice; the seam turns the fetch fault there
into `rt_sigreturn`, as the gadget runtime does.

## Guest-visible model

- CPUID comes from `../cpuid.hpp`, with nothing withheld; AVX and AVX2 run as
  pairs of 128-bit operations on hosts without 256-bit SVE. x87 is the full
  80-bit format. RDRAND and RDSEED read the host CSPRNG through a helper and
  always succeed.
- An encoding the decoder or the dispatcher refuses raises `#UD`, never a host
  failure, as do FEXCore's reserved escapes 0F 3E and 0F 3F and AMD's 3DNow!,
  FEMMS, SSE4a and CLZERO, as on an Intel core. The system instructions follow
  the gadget engine's
  [model](../gadgets/README.md#guest-visible-model): the privileged ones raise
  `#GP` before they touch memory.
- SIMD floating point gives x86's results whatever the host's features. The
  maintained series follows each x86 SIMD floating-point operation with an
  `EmbedderFPCheck` op that tests the host's result in line, and a cold path
  hands the cases where arm64 arithmetic can differ to `../fp_model.cpp`, which
  computes x86's result in integers and SoftFloat. FEXCore's FEAT_AFP path
  stays off ([optimization history](../../../../docs/optimization-history.md#floating-point-control-features)).
- A translation assumes its context's floating-point mode: MXCSR.DAZ and FTZ,
  which exceptions MXCSR unmasks, whether the x87 control word unmasks any, and
  RFLAGS.AC. An instruction that changes the mode ends the dispatch after
  itself, and the next one runs in the new mode's space.
- MXCSR, the sticky exception flags, the three flags that stay inexact
  (`x86-64-jit-masked-flags`), the exceptions a guest enables (`#XM`, `#MF`,
  `#AC`), the x87 transcendentals and the known deviations are the gadget
  engine's: see its [floating-point environment](../gadgets/README.md#floating-point-environment)
  and [exceptions](../gadgets/README.md#exceptions-the-guest-enables). The JIT
  checks `#AC` on every access of an instruction where the gadget engine checks
  the first.

## Memory order

x86 keeps every store after the stores before it and every load after the
loads before it. The JIT keeps that order for an address space the kernel marks
shared (`DARWIN_UML_DISPATCH_MM_SHARED`, set as the
[common kernel README](../../../../kernel/common/README.md) states) and drops
it elsewhere, where no other agent can tell. An ordered space is a FEXCore
context with TSO on, and the engine fences each side of a REP MOVS or STOS; an
unordered space uses plain loads and stores. MFENCE, locked instructions and
XCHG stay full barriers in both. The mark alone decides; no setting overrides
it. A dispatch the kernel marks shared that runs unordered code breaks the
contract, and the runtime logs it. `UML_X86_MM_SHARED_LOG` logs each change of
the mark. `x86-64-jit-store-order` runs message passing between two engines on
two host threads and would notice the order going missing.

## Build and test

```sh
cmake --workflow --preset check-unit
```

On macOS the unit tree builds this engine. `ctest -R '^x86-64-(jit|tier)-'`
runs its tests. `x86-64-jit-instruction-corpus` runs the x86-64 instruction
corpus through the engine, and the pinned FEX i386 corpus and the compat32
`--retirement` cases go through it too.
[`jit_corpus_exclusions.tsv`](../../../../tests/unit/x86_64/jit_corpus_exclusions.tsv)
names each record the engine is known to fail with its class; the test fails
when a listed record passes or a row names no record, so a fix removes its
rows in the same change. `x86-64-jit-sse-exactness` runs the SIMD differential
harness against the same x86 model as the gadget engine's
`x86-64-sse-exactness`. The campaign is in
[`docs/testing.md`](../../../../docs/testing.md#x86-instruction-conformance).
