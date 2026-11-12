# x86-64 gadget engine

The engine runs x86-64 userspace, long64 and compat32, as data-only streams of
pointers to handwritten arm64 gadgets. It is the x86-64 product's default
engine and the only one on every other product that runs x86 code. This file
records the contracts that are not obvious from the code; the design credit for
both gadget engines is [`NOTICE.md`](../../arm64/gadgets/NOTICE.md).

## Ownership

| Path | Role |
| --- | --- |
| `engine.S`, `engine/*.inc` | The gadgets. `engine/region.inc` places every group. |
| `engine/generate_baked.py` | Writes the register-baked scalar bank (`baked_scalar.inc`) at build time. |
| `engine/fit_slots.py` | Reports the groups that no longer fit their slot. |
| `native_builder.cpp`, `native_baked.hpp`, `native_entry.hpp` | Turn decoded instructions into streams. |
| `executor.cpp`, `program.cpp`, `translation_body.hpp`, `translation_space.*` | The translation cache, its sharing and its budget. |
| `native_memory.cpp` | The C++ side of the software TLB. |
| `exception_checks.*` | Checked execution of the exceptions a guest enables. |
| `x87_math.*` | Bounded software primitives for x87 formats. |

The target is `uml_x86_64_gadgets`. The guest CPU state, decoder, CPUID,
software TLB entry, code-page registry, split-operand lock, segment model and
SIMD floating-point model are not the engine's own: they are
`uml_x86_64_target` in the parent directory, which every x86 engine links.
Gadget streams are data. Executable memory has one owner,
`darwin/cpu/jit_memory/executable_memory.cpp`, and this engine never allocates
or publishes code
([architecture](../../../../docs/architecture.md#cpu-execution-and-scheduling)).

## Engine selection

The gadget engine is selected by name and is the default. On macOS
`--execution-engine jit` sends long64 and compat32 tasks to the
[x86-64 JIT](../jit/README.md) instead; the runtime chooses per dispatch, so
neither engine depends on the other.

## Region layout

The engine's code is a pinned region, a bank and a cold part, each a run of
fixed slots from the table in `engine/region.inc`
([architecture](../../../../docs/architecture.md#cpu-execution-and-scheduling)
has the contract). Private labels take the Mach-O assembler-temporary prefix
`L`; a named local symbol ahead of a target fixes an addend the assembler
rejects.

- A group (the common entry code, the veneer island, one baked family or one
  `engine/*.inc` file) that changes size within its slot moves no other group.
- A group that outgrows its slot fails assembly at `.org Lx86_bank_end_<group>`
  or `.org Lx86_cold_end_<group>`, and every later group fails too.
  `engine/fit_slots.py --build-dir <tree>` lists each group that no longer fits
  with the slot it needs: its phase and new size plus 1 KiB, rounded up to a
  128-byte line. Setting those rows' slots moves later groups by whole lines
  and keeps their phase, so the commit says which slots grew and needs no pad
  control ([optimization history](../../../../docs/optimization-history.md)).
- A phase never changes. A new group takes a new row with phase 0: a new
  `engine/*.inc` file's at the end of the table, a new baked family's after the
  last baked one.
- A call to a helper outside the engine is a `helper_call`, and a new helper
  is appended to the island's list in `engine/common.inc`.

## Gadget banks

The ordinary bank decodes its operands from stream words and covers every shape
the decoder produces. The register-baked bank has every register offset as an
immediate in its entry; `engine/generate_baked.py` enumerates it,
`native_baked.hpp` declares the tables, and their enumeration order must change
together. The builder takes a baked entry for every 64-bit-mode instruction the
bank enumerates that has no segment override, address-size override or LOCK
prefix, and the ordinary bank for everything else.
`UML_X86_64_GADGET_BAKED=0` keeps the ordinary bank for every instruction, for
comparison. The descriptor bit `UML_X86_64_MEMORY_PLAIN_ADDRESS` is only ever
an optimization: a descriptor that leaves it clear takes the general address
tail and resolves the same address.

## Stream and block contracts

- An instruction whose gadget can fault, trap or leave carries its position
  (its address less the block's first address, `program_begin`) in ten bits,
  not its address. A cold exit rebuilds the RIP with `instruction_rip`, so a
  block ends before the first instruction 1024 bytes or more past its start. A
  baked PUSH or POP of a register carries nothing: `execute` recovers the
  instruction from the stream pointer on a fault.
- A block ends at an unconditional transfer or at the 64-instruction fetch
  window, not at its first conditional branch. A cached link names its target
  by a stream pointer and an instruction count, one aligned pair that a single
  paired store replaces; the guest bounds of a stream sit in the two words in
  front of it, loaded at `[stream, #-16]`. `x86-64-gadget-link-layout` holds the
  layout.
- A dispatch's instruction budget bounds how long it runs, not where a block
  stops: the block that crosses it runs to its end. Only a single step and the
  instruction a POPF or IRET setting TF leaves before its trap ask for an exact
  budget, and that replays the block from its saved source, never from guest
  memory.
- A repeated string instruction retires once however many elements it moves.
  It yields between elements after `UML_X86_64_STRING_REPEAT_BUDGET` of work
  with RIP still naming it, and the dispatch charges the same work to its
  budget. A step is one iteration. A repeated CMPS or SCAS commits its flags
  when it finishes.
- RF is state only: `execute` clears it as it enters the engine and no gadget
  reads or clears it. The runtime reports RF set for a fault exit and the live
  flags for a trap, as hardware does.
- The deferred-flag record (`DeferredFlagOperation`, `CpuState::carry_slot`) is
  read by assembly and C++ materializers that must agree. A producer that
  leaves host NZCV lets the next SETcc, CMOVcc or Jcc read it, and the deferred
  record is always written as well.

## Guest-visible model

- System instructions behave as at CPL 3 on Linux with UMIP: the privileged
  ones raise `#GP` before they touch memory, and SGDT, SIDT, SMSW, SLDT and STR
  store what Linux's UMIP emulation stores. RDPID, RDTSCP's ECX and the per-CPU
  segment limit report processor 0 of node 0. CMPXCHG16B raises `#GP(0)` off
  16-byte alignment, locked or not.
- Both x86 engines share one segmentation model
  ([`segments.hpp`](../segments.hpp)): Linux's GDT, the i386 TLS entries and the
  task's `modify_ldt()` LDT. Segment loads, far transfers, an IRET that changes
  CS or SS, and LAR, LSL, VERR, VERW, SLDT and STR leave the engine unretired
  for `execute_segment_instruction`. The execution mode follows CS, code
  segments are flat, and 16-bit segments are refused, so Win16 and DOS code do
  not run. In compatibility mode FS and GS accesses check their limit, and a
  task whose ES, SS or DS is not flat writable data steps one instruction at a
  time; long-mode code pays for none of it.
- The engine runs only on hosts with FEAT_LSE2, which the runtime checks once.
  A locked operand inside one 16-byte granule keeps the exclusive loop; one
  across a granule or a page runs under the process-wide `split_operand_lock`
  (`../split_operand_lock.hpp`), which is atomic only against other operands on
  that path.
- Every gadget that touches guest memory leaves through a `dmb ish`, because
  x86 promises total store order and full-fence LOCK and XCHG. `translate`,
  `stack_fast` and the bulk-helper macros mark their gadget
  (`x86_marks_guest_memory`); the mark is textual, so a new guest-memory path
  that bypasses them must mark itself.
- A translation may miss and a miss is a call that does not save host vector
  registers, so a gadget may not hold a guest value in one across its own
  translation. This is also why vector result forwarding takes register forms
  only.
- The software TLB is direct-mapped. `native_tlb_entry_count` in
  `../software_tlb.hpp` is the one constant; the three assembly copies of the
  index (`translate` and `translate_plain` in `engine/common.inc`, and the
  baked entries in `engine/generate_baked.py`) must change with it.

## Floating-point environment

A dispatch runs in the guest's rounding mode: entry maps MXCSR's rounding field
onto FPCR's and every MXCSR write republishes it. SSE, AVX and FMA arithmetic
return x86's values where the host's differ (NaN selection, DAZ and FTZ, the
estimate instructions), and MXCSR's exception flags are sticky through the
host's cumulative FPSR flags. Three flags stay inexact, as
`x86-64-masked-flags` names them: the denormal operand while DAZ is clear,
which no host flag shows, and two the host raises and x86 does not, the
underflow of a product, quotient, FMA, narrowing or dot-product step that
rounds up to the smallest normal and the invalid operation of an FMA of
infinity times zero plus a quiet NaN. x87 arithmetic computes in SoftFloat
helpers and raises no host flag; the transcendentals follow an Intel core, in
FEX's `cephes/X87Math.cpp`, which the JIT shares, and do not reproduce its
microcode rounding noise. The model of the exact SIMD flags is
[`../fp_model.hpp`](../fp_model.hpp).

`uml_x86_64_gadget_leave` restores round-to-nearest by clearing FPCR's mode
field. [`gadget_host_frame.inc`](../../gadget_host_frame.inc) lays out the entry
frame for both gadget engines. No gadget keeps a value in x18: Darwin clears
that platform register when it interrupts the thread, and `architecture-audit`
rejects one.

## Exceptions the guest enables

x86 raises `#XM` for a SIMD floating-point exception MXCSR unmasks, `#MF` at
the next waiting x87 instruction after an unmasked one, and `#AC` for a
misaligned access while RFLAGS.AC is set. A guest that enables none runs
unchecked. Otherwise a gadget leaves before it stores anything, with
`UML_X86_64_EVENT_EXCEPTION_CHECK` and the stream position, and the runtime
runs that instruction again alone and checked. When RFLAGS.AC, MXCSR unmasking
underflow, precision or a denormal operand, or a pending unmasked x87
exception makes every such instruction a candidate, the runtime dispatches
through an executor whose blocks end before each one (a check head that no link
enters, run by `execute_checked`). An instruction that could change what a run
checks, such as POPF setting AC or LDMXCSR and FLDCW unmasking, retires and
leaves the engine, so linked code never runs past it unchecked. Code under
these settings runs much slower than unchecked code.

Known deviations from x86 that remain:

- The faulting address of a partly mapped FXSAVE, XSAVE, XSAVEOPT or XRSTOR
  area is the first inaccessible byte or the operand's base, where an Intel
  core reports the last byte of the extent first.
- Neither engine raises the `#GP` x86 raises for a misaligned operand of an
  aligned SSE form such as MOVAPS, MOVNTPS or a legacy SSE 16-byte memory
  source.
- The gadget engine checks only an instruction's first access for `#AC`; the
  JIT checks every one.

## Code pages and cached translations

- [`../code_page_registry.hpp`](../code_page_registry.hpp) keeps, per
  address-space generation, the set of pages in which any executor holds a
  translation. A store publishes a code change only when its page is in that
  set, which keeps publication precise under READ_IMPLIES_EXEC. A participant
  with another TLB layout, such as the x86-64 JIT, joins with a function that
  retires its own entry for a page. A page enters the set before any
  translation built from it is handed out, and every participating executor's
  write-side TLB entry for it is retired before the registration returns.
- Every store to a page that holds cached code owes a publication of its own,
  and the program performs it before its next store: another vCPU may
  revalidate the page between two stores, so a program that patches a page
  store by store pays one publication per store.

### Keeping a cached translation

A cached program is reused while the guest bytes it was translated from are
still there. The stamp of each page it covers, which any code-change
publication on that page moves, and the program's saved source bytes, compared
whenever a stamp has moved, answer that. A stamp certifies only the bytes read
after it was taken, so a stamp is always taken before the source it stands for,
and a new translation reads its block once. A writer stores the bytes, then
moves the page stamp, then moves the code-change sequence; a reader observes
the sequence before it looks. A store still in flight reaches the vCPU at its
next code-change checkpoint (a system call, CPUID or IRET), which leaves the
native run so the runtime revalidates what it selects next. A moved mapping
sequence or a raised execution interrupt abandons a validation.

### Sharing and budget

- Executors that run one address space share its translations, unless
  `DARWIN_UML_GADGET_SHARED_BODIES=0` keeps them private. A shared space is
  made for an address-space generation and the exceptions its blocks are built
  for, and an executor built for other exceptions never shares with it.
- A body is unpublished when a slot lets go of it but stays in limbo until every
  executor seen inside translated code has left, which `run_x86_gadgets`
  announces with `begin_run` and `end_run`. A shared space is never swept: it
  is closed and replaced once its arenas hold as much freed space as live
  bodies, and dies with its last executor.
- Each executor lets go of the address spaces the MMU reports destroyed at the
  start of every run (`Executor::leave_gone_generations`); the kernel never
  reuses a generation number.
- Each address space charges an account in the gadget engines' process-wide
  byte budget (`cpu/gadget_cache_budget.hpp`,
  [`darwin/README.md`](../../../README.md)); an account asked to shed is let go
  of at the executor's next run.

`UML_GADGET_CACHE_CENSUS=ON` builds record what each cache holds in the arm64
engine's format
([`tests/perf/README.md`](../../../../tests/perf/README.md#cache-census)).

## Build and test

```sh
cmake --workflow --preset check-unit
```

Focused tests: `ctest -R '^x86-64-gadget-'` (cache, link layout, sharing,
bootstrap, pinned region), `x86-64-masked-flags` and `x86-64-sse-exactness`.
The instruction corpus, which is the acceptance gate, and the FEX campaign are
in [`docs/testing.md`](../../../../docs/testing.md#x86-instruction-conformance).
