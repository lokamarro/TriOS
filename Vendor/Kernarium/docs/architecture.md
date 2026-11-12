# Architecture

This document states the boundaries and the cross-module contracts an agent
must not break. Exact values (budgets, deadlines, sizes, option lists) live in
the header, script or test named beside each rule.

## Dependency direction

The enforced first-party dependency direction is:

```text
app -> emulator / device
emulator -> runtime / device / api
runtime -> cpu / memory / device contracts / platform
cpu -> memory / platform
memory -> platform
device -> memory / platform
```

`darwin/CMakeLists.txt` composes owning subdirectories. Each module owns its
targets and source manifest. The neutral CPU factory selects the gadget or JIT
adapter; engines do not depend on one another, and shared CPU code depends on
neither. `architecture-audit` enforces this on each CPU target's declared
engine kind and on textual includes between the engine directories.

An emulator archive is a self-contained final product: CMake merges object
libraries and interface-carried objects into it, never a linked static archive,
and `scripts/build-xcframework.sh` rejects a release archive that leaves a
first-party symbol undefined or exports `uml_emulator_` functions beyond the
public headers under `darwin/api/include/uml`. The assembly-visible CPU and TLB
layout is owned by `darwin/cpu/arm64/layout/assembly_offsets.h`; test-only hooks
belong in test/support targets.

## Runtime ownership

Each `uml_emulator` handle owns its runtime context, engine selection, stop
state, and copied network callback table, and admits at most one run at a time.
A stop request affects only that handle's current or queued run. Runs on
different handles serialize through a process address-space gate, because an
embedded kernel's fixed host mappings cannot be shared; a callback that
resolves no single run fails closed. The caller owns provider lifetimes; the
library has no process-global network provider.

A process runs one guest at a time. After a kernel has claimed guest RAM, a
later run is refused until the guest has been handed back
(`runtime/guest_hand_back`): automatically by the next run on a handle whose
guest restarted, or by `uml_emulator_release_guest` after a clean power-off or
halt. A failed or host-stopped guest cannot be released and requires a fresh
process. `uml_emulator_get_run_end` says how a run ended: the guest's own report
(see [guest lifecycle](#guest-lifecycle)), the host's stop request ahead of it,
or a failure when the kernel said nothing. Every run of a process uses the same
JIT cache budget and guest CPU count.

The hand-back refuses with `UML_EMULATOR_RUNTIME_ERROR` while a host service
borrows guest RAM or a kernel thread runs, and a refusal changes nothing.
Otherwise it gives back everything the kernel took and did not return: the
descriptors, allocations and mappings the loader recorded
(`runtime/kernel_resources`), the process state the kernel changed
(`KernelSignalScope`), the guest-file records, suspension and hibernation, and
the guest RAM, whose window becomes one protectionless reservation again. The
`guest-restart` test requires it to leave nothing in the engine's
[domain heaps](#domain-heaps) but the per-thread scratch.

## Memory and executable code

All products use a 128 TiB sparse software-MMU guest task range and 4 KiB guest
pages on the 16 KiB Apple host page. The native kernel window holds guest RAM,
a RAM-scaled auxiliary mapping allowance, and the guarded aperture and stub
pages the kernel reserves; host and kernel derive that reservation from the same
constants (`runtime/native_kernel_layout.hpp`). The window is a reservation that
never returns address space while the process lives: the first run reserves it
once (`runtime::reserve_process_native_window`) and every later run takes the
same window. Guest pages are mapped over it with `MAP_FIXED`, and releasing one
hands its range back as a reservation, never with `munmap`, which would let an
unrelated `mmap(NULL)` take the gap. Guest RAM that a host-service view still
borrows stays mapped until the last view is released. The linear physical window
is the anonymous RAM root; consumers borrow it or create non-copying Mach
aliases, and disks stay file-backed.

`mem=` is a ceiling, not a reservation: the guest commits RAM before using it
and releases what it frees (`darwin_uml_physmem_commit`/`_release`,
`memory/guest_ram_commitment.cpp`), and released pages leave the process
footprint. Physical iOS also wires a page while it is committed; the one window
with a held page unwired is a [hibernation](#hibernation). The kernel trims its
page cache only at the host's request (`uml_emulator_trim_page_cache()`, every
quiesce). The host GPU's memory is not carved out of the guest's RAM, and the
host provides no Virtio balloon device.

**Budgets.** The process's memory limit, L, is the figure the memory budgets
derive from. iOS enforces one per application
(`platform::process_memory_limit_bytes()`); macOS enforces none, so a
measurement run states it (`--memory-limit=`) and every budget computes on the
Mac as on the device (`runtime/virtual_jetsam.hpp` marks where iOS would have
ended the process). The
host GPU's budget, B, is computed once at launch and fixed for the run
(`platform/gpu_budget.hpp`, planned by `emulator/src/gpu_budget_plan.*`):

```text
B = clamp(L - A - G - E - S, 384 MiB, G / 2)
```

G is the guest's RAM, E the guest side's host memory, A the application's
expected ceiling and S the unused headroom; with no limit B is G / 2. A, the rest
of E and S are launch parameters defaulting to the conservative launch set in
`runtime/guest_memory_budget.hpp`. L is the limit the system advertises to the
process; shortage elsewhere in the system is not counted. A launch under the
floor is never refused: B is the floor and the launch says the process is likely
to be ended. [`graphics.md`](graphics.md) owns what B bounds.

The guest side's memory follows the vCPU count, v, and that header is the one
place that says how; every caller evaluates it:

```text
E(v, G) = c(v, G) + 4 MiB * v + E_rest
c(v, G) = min(derive_budget(G, L), cap(v))      where L is set; derive_budget(G, L) alone otherwise
```

`cap(v)` is a table of the gadget cache's ceiling per vCPU count. The largest
guest RAM a limit leaves, `G_max(v)`, is the largest G (in 64 MiB steps, within
the bounds in the header) that keeps B at its floor or above. The settings of
Kernarium and Andarium offer no more than `G_max(v)` under a set that counts
none of A, E_rest and S (`maximum_memory_set`). The conservative size is
`G_max(v)` under the conservative launch set, in 0.5 GiB steps within 1.5 to 4
GiB and never above that maximum (`conservative_guest_memory_mib`); a guest with
more is likely to be ended by the system, and Andarium says so on Home and
beside its memory picker ([Andarium](../Andarium/README.md)). A new guest's RAM
is the conservative size (`default_guest_memory_mib`), and on a device whose
maximum at one vCPU is the top of the range, 4 GiB, at every vCPU count also no
more than the largest 0.5 GiB step that leaves B at 1 GiB or more, since a 4 GiB
guest otherwise runs the host GPU short long before it runs the process short;
that step is never under 2 GiB, because B is at most half of G and every limit
that offers 4 GiB at one vCPU leaves a 2 GiB guest more than 1 GiB at every
count. A guest with more than the default but no more than the conservative size
is likely to run out of graphics memory, and Andarium's System sheet says so
beside its estimate of B for the chosen RAM and vCPUs (`gpu_budget`). The macOS
launchers take the same default for a launch that states a memory limit. The
applications' vCPU limit is the rule `application_vcpu_limit` in the same
header: the host's physical cores, at most 8. It is the default and the maximum
of Kernarium, Andarium and the macOS launchers (`emulator/src/options.hpp`); an
explicit `ncpus=` on a launcher keeps the runtime's own range. Every iOS memory
control has a macOS counterpart, so a Mac run stays an accurate ground for
measuring the device.

A kernel-range flush completes or the kernel stops: `flush_cache_vmap()` and
`flush_cache_vunmap()` are both `flush_tlb_kernel_range()`, its result is never
discarded and a persistent host failure panics, because nothing faults on a
mapping that should not exist. Software-MMU mappings are published only by a
page-table walker's batch, which is rejected when an invalidation arrives before
it ends; a 16 KiB host slot is a shared host page only when its four PTEs name
one aligned run of a single backing (`arch_vmap_group_order()`).

### Domain heaps

The default malloc zone returns what a restarted guest freed on its own
schedule, so a guest's metadata (the MMU's indexes, the native address space's
state, the engines' program and record tables, batch plans) stays out of it.
`memory/domain_heap.hpp` provides per-domain heaps carved from one reserved
region and released whole when their owner retires, and thread-local scratch for
one operation's temporaries. The GPU stack, Metal, system frameworks, devices and
the process's own objects stay in the default zone, whose residue is what
remains.

### Executable code

Gadget streams, MMU backing, and shadow mappings are data. Only
`darwin/cpu/jit_memory/executable_memory.cpp` may allocate executable memory,
publish RW pages as RX, control JIT write protection, or publish the
instruction cache; the arm64 JIT arena and FEXCore (through hooks the embedder
installs) reach it only through that file, and the `native` engine kind and the
source audit keep the gadget engines and shared CPU code from linking or
including it. The one exception is Mesa llvmpipe's own LLVM JIT in the iOS
Simulator product, whose GPU prefix alone carries LLVM; prefix verification and
packaging reject it in the macOS and iOS device slices. On macOS the owner serves
JIT memory from a window near the host image, because generated code calls into
it and a far buffer made x87-heavy FEXCore code about a third slower; memory
served again may hold its old bytes.

Every product that embeds a kernel carries its ELF as a host-page-aligned
Mach-O `__TEXT,__uml_elf` section, which the emulator runs in place: the loader
aliases the immutable executable pages, copies only data and BSS, and rejects
writable executable segments or relocations into signed text. On iOS the image
must be that section, because no other code can be made. The loader admits only
an AArch64 image, so the x86-64 userspace of the macOS compatibility product is
translated by its engines, never loaded. Kernarium's app embeds every selectable
runtime framework, links none at launch, and loads exactly one guest flavour for
the process lifetime.

The macOS, iOS device, and iOS Simulator products build at a 27.0 minimum
(`cmake/toolchains/Apple.cmake`), so their code carries no availability guard or
fallback for an older release. Apple products consume accepted kernel bundles
and never invoke Kbuild: one mainline artifact for arm64 userspace on macOS and
the iOS device, a separately identified 4 KiB mainline slice for macOS x86_64
userspace, and a separate accepted 4 KiB `android-6.18` artifact (the same
mainline tree with the Android configuration) for Android. Kernarium and
Andarium are iOS products ([`../Kernarium/README.md`](../Kernarium/README.md),
[`../Andarium/README.md`](../Andarium/README.md)); the device slice also runs on
an Apple silicon Mac, and the Simulator has a UI-test variant with the
CPU-rendering GPU prefix ([`graphics.md`](graphics.md)).

## CPU execution and scheduling

Darwin owns one persistent pthread per configured vCPU, including vCPU0
(`uml-vcpu-N-carrier`). Linux task contexts remain on guest kernel stacks and
migrate between carriers; guest task creation does not create host threads. An
embedded run returns to the loader, which unmaps the image, only after the
kernel has joined every carrier and stopped its RTC alarm helper.

**Exit contract.** A host can call `exit()` while carriers still run, as an iOS
application and an XCTest host do. State that carriers use until their process
ends therefore has no exit-time destructor: it is constant-initialized or never
destroyed. That covers the arm64 gadget contract and twin indexes, the
shared-backing state, the active-access registry, the gadget cache ledger, the
MMU's thread-slot and domain state, and the console relay's and loader's output
registrations and locks (`arm64-gadget-exit`).

Darwin signals wake carriers but do not execute Linux IPIs asynchronously;
IRQ-enabled spin checkpoints service pending IPIs. Early UML calibration uses
process-directed `SIGALRM`, which application and launcher threads keep blocked
and only the vCPU0 carrier unmasks; Kernarium runs the guest on its
`uml-launcher` thread, never the main thread. The embedded kernel's signal
handlers and interval timer outlive its entry, and Darwin never moves a
process-directed signal off the thread it chose, so before it releases the image
the loader disarms `ITIMER_REAL`, discards pending instances of every signal
whose handler lies in the image, and reinstalls the host's dispositions and mask.

The product has two selectable engines: the gadget engine executes arrays of
precompiled function pointers and data, and the JIT emits native blocks through
its sole executable arena. Guest-visible exceptions, fault priority, retirement,
and feature exposure must agree across engines and with the independently
derived expectations of the A64 instruction corpus. Instruction classes the JIT's
native lowering leaves to a helper run in the JIT's own semantic slow path, never
in another engine, and a helper treats any word outside its routed classes as a
fatal internal error. Each engine's internals are in its README
([gadgets](../darwin/cpu/arm64/gadgets/README.md),
[JIT](../darwin/cpu/arm64/jit/README.md)); FP trap-enable semantics are decided
for both by `darwin/cpu/arm64/detail/floating_exception.hpp`.

The arm64 gadget code is one region in `darwin/cpu/arm64/gadgets/engine.S`: the
bank, the sections its gadgets push variants and cold paths into, and an island
of veneers. Every image that carries it links `UML::GadgetRegionLayout`, which
orders it first in `__TEXT,__text`, so the region starts on a 16 KiB page, each
part starts on its boundary plus the phase `engine.S` defines, and its bytes
depend on `engine.S` alone. Private labels take the Mach-O assembler-temporary
prefix `L`, and a branch to one in another part encodes its distance past the
nearest named symbol as a signed 24-bit addend, so such a target must lie within
8 MiB of one (`gadget-engine-temporary-labels`). The bank holds no padding: a new
entry or family is appended after the established ones of its part, while a
removal moves every later entry and is measured on the iPad like any gadget
change (`arm64-gadget-region-layout`). The x86-64 gadget code is pinned the same
way, a bank and a cold part of fixed slots, one per group, from
`engine/region.inc` (`x86-64-gadget-region-layout`; the
[x86-64 gadget README](../darwin/cpu/x86_64/gadgets/README.md) says what a change
moves).

Data-alignment faults retain the effective data address and the faulting PC,
and every kernel product delivers `SIGBUS/BUS_ADRALN` so a guest handler can
repair the access and resume. Both engines advertise USCAT exactly when the host
implements FEAT_LSE2; then a misaligned LSE, exclusive or acquire/release access
inside one 16-byte block is single-copy atomic, one that crosses a block faults,
and without host LSE2 every such access faults.

**Exclusive monitor.** A load/store-exclusive loop makes progress whatever the
TLB holds, and nothing an engine does between the pair without an exception
fails the store. An exclusive load fills the write TLB entry and records the host
address and loaded values in the `CpuState` exclusive fields
(`engine/exclusive.inc`, reused by the AArch32 exclusives), and a store-exclusive
that still misses completes in software with one host compare-and-swap of the
recorded values. The JIT's semantic path keeps a software reservation (versions
of 16-byte host granules), so a store to the reserved granule, including the
kernel's futex and SWP emulation, fails the store-exclusive. The monitor ends at
a store-exclusive, a CLREX, an exception and a store to the reserved granule; a
store of the guest's own to another granule leaves it standing, as on hardware
(`arm64-exclusive-progress`, `arm64-exclusive-events`,
`arm64-jit-exclusive_completion`, `guest-exclusive-progress-gadgets`/`-jit`).

Both engines complete WFE and WFI at once and treat SEV and SEVL as no-ops, so
no wait depends on an event. Completing WFI deliberately deviates from arm64
Linux, which kills the program with SIGILL; AT_HWCAP therefore always carries
EVTSTRM.

### SME and SVE

The SME profile (FEAT_SME with FEAT_SME_F64F64 and FEAT_SME_I16I64, plus
FEAT_SME2 where the host has it) runs at the host's streaming vector length,
because native execution needs the guest's SVL to equal the host's;
[`cpu/arm64/sme_profile.hpp`](../darwin/cpu/arm64/sme_profile.hpp) derives it
for both engines. It is on by default wherever the host has it
(`DARWIN_UML_GUEST_SME=0` forces it off). An SVE or SME word an engine does not
implement is the illegal-instruction exit, and both engines are `complete`, so a
guest is told of SME1 only. Only the M4's SVL of 64 bytes has been checked on
hardware, and the iOS SME path is unvalidated on hardware.

The virtual CPU has no SVE outside streaming mode and advertises none
(`platform::without_sve`); every SVE word outside streaming mode, other than the
family below, is the illegal-instruction exit in both engines, so a binary that
computes with SVE there takes SIGILL. One deliberate departure covers a CPU with
FEAT_SME: CNT[BHWD], RDVL, ADDVL, ADDPL, the scalar INC and DEC forms, and LDR and
STR of Z and P registers with a scalar base and MUL VL offset execute at
PSTATE.SM = 0 for one constant vector length of 128 bits. The architecture makes
them UNDEFINED there, but compilers target armv9-a, so a locally-streaming
function reads the non-streaming vector length first and would take SIGILL on
every SME host. The family keeps SME code running and is no promise to run SVE
code;
[`cpu/arm64/detail/sm0_vector_length.hpp`](../darwin/cpu/arm64/detail/sm0_vector_length.hpp)
defines it for both engines, and the corpus holds the results as project-defined
(`arm64-sm0-vl`).

### AArch32 and top-byte ignore

Both arm64 engines run a 32-bit Arm guest: AArch32 EL0, A32 and T32 with
interworking and IT, for the armhf userspace of Debian and Android's
armeabi-v7a, which needs hardware VFPv4-D32 and Advanced SIMD. The profile is in
[`cpu/arm64/aarch32/a32_profile.hpp`](../darwin/cpu/arm64/aarch32/a32_profile.hpp)
and every arm64 product presents all of it with no switch;
`CpuEngine::run_aarch32` is the entry for a task in AArch32 state. The PC is a
translation key
([`cpu/arm64/aarch32/guest_key.hpp`](../darwin/cpu/arm64/aarch32/guest_key.hpp))
that carries the ISA, so one gadget cache, JIT arena and shared tier serve A64,
A32 and T32. The compat `AT_HWCAP`/`AT_HWCAP2` words derive from the host the
selected engine presents, and a non-zero word is the kernel's whole evidence
that a run executes AArch32. An entry in a state the kernel may not hand over is
fatal, as is a fault address wider than 32 bits, which must never become a guest
SIGSEGV whose compat siginfo would hide the upper half.

One decoder serves every AArch32 consumer: `darwin/cpu/arm64/aarch32` decodes
A32, T16 and T32 words from a table generated from the pinned Arm AArch32 XML,
and whatever the Armv8-A AArch32 EL0 profile lacks is undefined, never guessed.
It is a CPU-layer `shared` target that includes no engine
([README](../darwin/cpu/arm64/aarch32/README.md)). The semantic library
([`cpu/arm64/aarch32/semantics.hpp`](../darwin/cpu/arm64/aarch32/semantics.hpp))
is shared C++ semantics, not another engine: it works on `CpuState` alone and
reaches guest memory only through the caller's `A32Memory`. The engines route
their slow paths to it, and a word outside `a32_semantics_covers()` is a fatal
internal error.

Under top-byte ignore, which every arm64 run has unless the host passes
`--guest-no-top-byte-ignore`, both engines strip the top byte of a data address
before they translate it, so a data abort carries the translated address. The
kernel therefore does not untag the fault address in `segv()`, and offers the
tagged-address ABI only while the host reports the ignore; the contract is in
the [shared kernel README](../kernel/common/README.md).

### x86-64 userspace

The macOS 4 KiB x86 compatibility slice keeps native arm64 UML kernel-mode
execution while exposing the x86_64 and i386 userspace ABIs. It cannot load
arm64 userspace; per-task mode selects only an x86-family operand and address
width. The host routes userspace instruction execution through immutable,
data-only streams of pointers to precompiled handwritten arm64 gadgets, with no
semantic C++ fallback ([gadget README](../darwin/cpu/x86_64/gadgets/README.md)).
What any x86 engine shares with the gadget engine forms `uml_x86_64_target` in
`darwin/cpu/x86_64/`, a `shared` CPU target that the source audit keeps free of
engine includes. The pinned FEX handwritten corpora are the external
instruction acceptance gate, which is not proof of an arbitrary distribution or
of complete Linux syscall compatibility; AVX-512 and later, RTM/MPX, 16-bit code
segments and privileged or virtualization instructions are outside the profile.
The normal mainline slices and Android remain arm64 kernels with none of the
x86_64 kernel overlay.

On macOS the product can instead run its tasks on the x86-64 JIT, FEXCore
embedded behind the static MMU (`--execution-engine jit`; the default is the
gadget engine); the [JIT README](../darwin/cpu/x86_64/jit/README.md) records its
contracts. The JIT is a macOS product only: the `macos` dependency prefix alone
carries FEXCore, configuration refuses `UML_BUILD_X86_64_JIT` elsewhere,
`architecture-audit` rejects the JIT target and any FEXCore link fragment outside
a macOS tree, and `scripts/check-no-fexcore.py` fails the iOS packaging on any
FEXCore symbol.

### Translation caches and guest debugging

The JIT's shared translation tier holds immutable guest identity and
relocatable code only, links are scoped to an address space, and no persistent
executor survives its runtime memory domain. The gadget cache tags slots and
links by address space and reuses a same-generation stale program only after
exact page-bounded word validation. Sweeping, renewal and the process byte
budget are in the header comments of `darwin/cpu/gadget_cache_geometry.hpp`,
`darwin/cpu/gadget_shared_space.hpp` and `darwin/cpu/gadget_cache_budget.hpp`,
summarized in [`../darwin/README.md`](../darwin/README.md).

To single-step a task, Linux sets the architectural step bit in the entry
registers (arm64 PSTATE.SS, x86 RFLAGS.TF); every engine then retires exactly
one instruction and reports a single-step exit, which Linux delivers as
`SIGTRAP`. Linux writes another process's memory (ptrace, `/proc/pid/mem`,
`process_vm_writev()`) through its own view of the frame, so no translated store
publishes the change: those paths call `darwin_uml_vm_invalidate_code()` for the
target mm's generation.

## Kernel and Android ownership

`kernel/mainline-6.18` owns the pinned source identity, configuration,
generated patch series, and standalone build policy of both the Linux and the
Android kernel products. `kernel/common` owns only contracts shared across
families. The Apple CMake graph imports compact artifacts and checks their
bridge ABI, product family, and `6.18` release series; the manifest admits only
the 4 KiB guest page size. Patch releases, commit identities, and producer
provenance do not become runtime compatibility keys.

`android/lineage-24` owns the Android product, static GPT layout, ramdisks,
rootfs release, and producer tools. Kernel and rootfs releases are selected
independently. Apple emulator builds consume only the Android kernel artifact;
rootfs and command-line paths are launch inputs. The `uml-darwin-android17`
product opens one directly writable supplied GPT.

Guest bundles, disk capacity, and compressed rootfs delivery belong to the
application ([`../Kernarium/README.md`](../Kernarium/README.md)). Runtime
frameworks receive an ordinary disk image and no capacity policy, and reject a
GPT whose declared extent differs from its file; the guest only grows a
filesystem after observing that its containing partition is larger.

## Devices and graphics

The vhost-user transport in `darwin/device/vhost_user` owns connection, queue
framing, and deferred used-ring publication. GPU, video, input, sound, console,
battery, and network providers own their protocol behavior independently. Only
the GPU provider depends on `UML_BUILD_GPU`; the others build on the transport
alone. A disabled ring is given no new data: a buffer the backend only fills (a
virtio-input event) stays in the ring until it is enabled again, and a buffer
that carries a request is consumed and dropped, because its sender may be
waiting with interrupts off; only the GPU, whose requests are asynchronous, asks
to keep them.

**Request gate.** A service can be given a request gate, which another thread
closes to hold the device's requests back: a closed gate lets no request reach
the device, waits a bounded time for the one the device is running, and leaves
later requests in their rings. Kernarium closes the GPU's gate as it enters the
background, because a request that reached Metal after iOS revokes the host GPU
would make the renderer's Vulkan device lost for good. A guest that ends cannot
wait behind a held gate: the kernel reports its end through
`darwin_uml_guest_shutdown()`, the devices in the guest-end registry
(`darwin/device/guest_end/guest_end.hpp`) are told, and the GPU service turns a
closed gate to discarding, so every request completes with the device's answer
for a request that did not run. `startNextConnection()` returns the gate to a
plain hold for the next guest.

The [Virtio video devices](../darwin/device/video/README.md) bridge the shared
V4L2 drivers to the platform VideoToolbox decoder and encoder over their own
vhost-user connections and need no GPU context. The
[Virtio vsock device](../darwin/device/vsock/README.md) carries a byte stream
from the host application to a service in the guest (adbd) with no listening
socket on the host; the guest cannot open a connection to the host.

**Lost backend.** A virtqueue call or kick descriptor, or the backend request
stream, that reports end of file or an error other than would-block means the
backend is gone. The guest stops that interrupt source at once, reports it once
per queue, and hands the device to the connection-broken path, which breaks its
virtqueues and unregisters it so that pending and later requests fail instead of
waiting (`guest-vhost-user-backend-gone`). Recovery ends there: a lost GPU
backend kills the guest's Vulkan client within seconds, but a client waiting on
a full control queue stays blocked until Mesa's own ring timeout.

**Control socket.** A queue is kicked by a request on the control socket. A
kick may ask for no answer, so that the kicking vCPU, which holds the socket
with its interrupts off, never waits for queue work. A backend answers a kick
that asks after it has run the queue, except the GPU and sound backends, which
answer first; while those two run a queue they tell the guest not to kick
(`VRING_USED_F_NO_NOTIFY`) until it has run dry. The guest endpoint is blocking
with no deadline, because `virtqueue_notify()` latches a virtqueue broken for
good when a notification reports failure; a backend whose answer never comes,
or is malformed, is gone and retired by the lost-backend path. Every other
request has a reply and only their order pairs them, so the guest holds the
socket from a request until it has read the reply and serves the backend
request stream during every synchronous wait.

The default Apple graphics path, from guest Mesa through Virgl and virglrenderer
to Zink on KosmicKrisp and the shared Metal presentation, is in
[`graphics.md`](graphics.md). GPU commands and rendering stay off the UI thread;
the shared presentation layer keeps the newest frame and at most one command
buffer in flight; only unsupported resource shapes use CPU readback. A guest
drives the renderer, so no host write into guest memory is sized by the renderer
alone: the device bounds a translated guest range to one negotiated memory
region, never writes a reply past the writable descriptors the guest supplied,
and the renderer ends a Venus reply stream at the window the guest declared.

Every GPU build also carries an independent Vulkan route, which the host always
advertises and which does not replace or enter the GL facade:

```text
Linux Vulkan -> Mesa Venus -> virglrenderer capset 4
    -> directly linked Mesa KosmicKrisp -> Metal
```

Its fences ride the same deferred fence pipeline as the default path. A fence the
renderer does not retire never costs the guest its GPU device: the first expiry
releases the ordering that held back guest commands and grants a second window,
and only a second expiry retires the fence without its renderer work, so a
wedged submission fails the workload that owns it while the guest can finish and
power off.

Venus keeps shareable Zink scanouts in their renderer-optimal Metal layout. For
a guest whose buffer allocator is not the Vulkan driver, the renderer gives an
imported GL resource a host shared-memory segment of its linear layout and hands
out the descriptor; a row is padded to a multiple of 16 bytes, the guest
allocator pads by the same rule, and ordering between the consumers is the
guest's own fences.

The in-process GPU service keeps the display apart from the guest's GPU
connection. The display (the VNC listener, the native window or Kernarium view)
lasts until the service stops; the connection (the Virtio-GPU device, the
renderer, and every context and resource the guest created) lasts as long as one
guest. A guest that restarts itself is a new guest on the same machine, so
`startNextConnection()` ends the connection, resets the display to its idle
picture and opens a fresh one, while clients stay connected. The request gate
belongs to the service, not the connection. Nothing enforces that a new guest's
host-visible blob starts zeroed.

### Local stream sockets

On Apple hosts, never wait for the end of a local (`AF_UNIX`) stream socket
with a blocking `read` or `recv`. Wait for readiness first (`poll`, kqueue or a
Dispatch source), then read, or read non-blocking after readiness. An end made
by `shutdown()` can lose the wakeup of a reader that is just going to sleep:
the peer's `SHUT_WR` or `SHUT_RDWR`, and a `shutdown` of the reader's own end
used to wake it. The reader then stays blocked although `poll` reports the end,
and with `SO_RCVTIMEO` the loss shows as a false `EAGAIN`. A `close()` by the
peer is not affected, and a data wakeup is never lost. Python's blocking
`socket.recv` is exposed in the same way; after `settimeout()` CPython polls
first, which is safe.

## Guest lifecycle

The platform layer tracks every writable host descriptor opened for the guest.
`uml_emulator_sync()` asks the guest to write back through its management
console, then flushes those descriptors. `uml_emulator_quiesce()` has the guest
trim its cold page cache and hand its free memory to the host, enters full Linux
system suspend and waits for the platform sleep boundary before flushing the
host. `uml_emulator_quiesce_resident()` does the same and keeps the guest's RAM
in the process where a quiesce would [hibernate](#hibernation) it, for a host
short of the space an image takes. `uml_emulator_resume()` sends the console
wake edge, and
`uml_emulator_wake()` sends it to a guest that is not quiesced, as a phone's
power key does. Every console request ends the suspend; the
[shared kernel README](../kernel/common/README.md) gives the kernel's side.

The suspend request has one bounded deadline; a refusal, or a guest that stays
busy, is not frozen and the host treats it as not suspended. A request with no
reply at the deadline is aborted: only a "suspended" reply that arrived before
the wake edge counts, otherwise the suspension is unconfirmed, never
hibernated, and woken at the resume. The management console's own stop is no
fallback, because it halts only one vCPU. Guests that need write-back boot with
magic SysRq enabled; if the physical iOS sandbox cannot bind the console,
host-file flushing still runs but guest write-back and freeze are unavailable.

The kernel says how the guest ends before it shuts its drivers down, with the
bridge import `darwin_uml_guest_shutdown()`: power-off, halt, or restart with the
command the guest passed. The runtime keeps the kind and a bounded copy of the
command in the run's `Context`. A panic reports nothing and aborts the process
after the runtime moves the kernel frames on every thread's stack and registers
from the kernel's alias to the same instructions in the framework's
`__TEXT,__uml_elf` section (`runtime/kernel_crash_frames`), where the runtime's
dSYM, which alone names the kernel's functions, symbolizes them in a crash
report. The panicking thread takes the panic's message as its name, which the
report shows.

A launch that passes `--session-record=PATH` gets a record of how its session
ended (`runtime/session_record`): "running" from the boot on, overwritten by
every end that runs code, a panic with its message and the kernel's last
output included. A record still "running" at the next launch means the process
was ended from outside without running, as a memory-limit end is. Its first
4 KiB are a memory checkpoint that a thread of its own rewrites every 2 s
(`memory_checkpoint_interval_ms`): the footprint, its peak and what the
process's memory limit still allows, the large terms of the footprint (guest
RAM, the host GPU's use against its budget, graphics, the gadget cache, the
domain heaps and malloc) now and at the peak, and a bounded history of them.
Reading malloc's term stalls every thread that allocates, so it is read only at
the first checkpoint and when the footprint climbs 64 MiB past the highest
reading's (`memory_malloc_reading_due`); every checkpoint also writes the
footprint less the other terms. Andarium names the file in its boot request and
reads it at launch.

A guest that restarts itself boots again in the same process, because iOS can
neither re-exec nor respawn an application. The macOS launchers
(`darwin/app/macos`) make the machine once (the emulator handle, the SLiRP
provider, the GPU service with its display) and make the services and command
line of each boot afresh; after a restart they reset the SLiRP link, ask the GPU
service for its next connection and run the handle again, and the exit status
is the last guest's. A guest that does not boot is not restarted for ever
(`emulator/src/restart_loop_guard.hpp`), and a stop request ends the loop at
once. Android's boot control block is part of the loop: every restart reads the
misc partition again (see the
[Android boot contract](../android/lineage-24/README.md)). The Kernarium runtime
runs the same loop inside its one `run` call
(`Kernarium/Runtime/KernariumRestartLoop.hpp`, `kernarium-restart-loop`): a stop
that arrives as the guest restarts wins, the next boot waits while the guest is
paused or the GPU request gate is held, and a restart is reported twice so the
application can re-apply what belongs to a kernel. After Android powers off
cleanly the runtime releases the guest through `uml_emulator_release_guest` and
returns success only once another run can start. An application that can be
backgrounded holds the guest's GPU requests (`set_gpu_hold`) before it quiesces
and releases them before it resumes.

The kernel's whole-file disk locks pass through the host's guest-file registry
(`platform/guest_files`). On iOS, iPadOS, the Simulator and in Designed for
iPad the registry keeps them itself and locks no host file, because iOS
terminates a process that holds a file lock when it suspends it, and a guest
that refuses or outlasts its suspend is still running then; macOS locks the
files. Once the guest confirms system suspension and its host files are
flushed, quiescing holds every thread that can run the guest and releases the
disk locks. The hold keeps the unlocked disks unwritten; a hold that cannot be
taken leaves the locks in place. Resume and stop reacquire every lock before
restoring guest RAM, releasing a thread or waking the guest, and verify the
disk's identity, size and times; contention, a replaced descriptor or changed
storage ends the process without resuming the guest.

The guest binds the management console as a Unix datagram socket under a
per-run `uml-kern<pid>` directory (mode 0700) that the host passes as `uml_dir=`.
The run removes it when it returns and on every terminating signal it observes;
the next run removes the directories of runs that have exited. Every such path
must fit Darwin's `sun_path` and is refused, not truncated, when it does not
(`guest-host-residue`).

### Statistics

An application that shows a running guest's load reads it from the host, and
nothing runs between reads: there is no sampler, timer or counter of its own.
`uml_emulator_get_statistics()` sums the user and system time of the process's
`uml-vcpu-N-carrier` threads and reports the guest RAM the host holds.
`uml_emulator_get_guest_memory()` asks the kernel through the management
console's `mem` command; like every console request it ends a suspend, so it is
sent only under the handle's lifecycle mutex (taken with `try_lock`) and only
while the guest is not quiesced. The macOS launchers print the same figures with
`--stats=<ms>`. The runtime's reading also carries the display's count of
refused graphics allocations and the device memory budget with its usage, and
a separate reading lists the budget's holders by guest process (the device
memory budget in [`graphics.md`](graphics.md)).

### Hibernation

On a physical iOS device, where guest RAM is wired and so neither compressible
nor reclaimable, a quiesce that entered system suspension also hibernates the
guest's RAM (`runtime/guest_hibernation.*`), so that the paused application is
small enough for the system to keep. `--guest-hibernation=auto|off|on` selects
it; `on` forces it on any host, which is how tests reach the same code. The
order is the contract:

- **Hold.** Every thread that can touch guest RAM is Mach-suspended first: the
  host's `uml-*` services and every thread the kernel creates
  (`runtime/kernel_threads`). A thread is held only where it holds none of the
  allocator's, C library's or runtime's locks; otherwise it is let go and tried
  again. If every thread is not held within a bounded time, nothing is held or
  written and the guest stays wired and resident.
- **Write.** The held pages go to an unlinked image in the application's
  `$TMPDIR` in hashed chunks, each taken off the footprint once written. The
  file holds no lock, because iOS terminates an application that holds a file
  lock during suspension.
- **Restore.** `uml_emulator_resume()` restores and verifies every chunk,
  releases the threads, and only then wakes the guest; `uml_emulator_request_stop()`
  restores first, since a held thread cannot see a stop.
- **Failure.** A restore that cannot bring every page back verified wakes
  nothing and ends the process with a diagnostic, so the next launch boots the
  guest again and Kernarium reports the restart notice.

A quiesce whose guest did not confirm its suspend, a guest without a console,
a resident quiesce, and a guest kept running (Picture in Picture, the job) never
hibernate. The first hold stands: a hibernated guest is not made resident by a
later quiesce, nor a resident one hibernated. A confirmed system suspension holds the same threads on every host whether or not
the RAM is written, because the disk locks leave with it (see
[guest lifecycle](#guest-lifecycle)).

## Tests and structural audit

Test suites and workflows are in [`testing.md`](testing.md).
`architecture-audit` checks the CMake File API graph, dependency cycles,
implementation includes, source ownership, and test registration. Only a
darwin/ source's owner compiles it: no other CMake file and no Xcode project
names one, and Xcode, not CMake, compiles the sources of Andarium/ and
GuestHost/. No host source under darwin/, Kernarium/, Andarium/ or GuestHost/
names x18 outside a comment, because Darwin clears that platform register when it
interrupts a thread (`arm64-emitted-registers` and
`x86-64-jit-emitted-registers` hold emitted code to the same rule). The audit
does not prove performance, generated code shape, a kernel boot, or guest
behavior.

The host/kernel ABI source is [`../kernel/common/abi`](../kernel/common/abi/).
Version, fingerprint, generated-copy, layout, and import checks fail closed on
drift.
