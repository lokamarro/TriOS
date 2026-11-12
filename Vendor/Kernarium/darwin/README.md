# Darwin UML host runtime

The arm64 Apple host that boots User-Mode Linux kernels: platform
capabilities, guest address spaces, the CPU engines, the devices, and the
products that compose them. [`../docs/architecture.md`](../docs/architecture.md)
owns the dependency direction and the runtime contracts that cross modules.

## Ownership

```text
api/       installed C API and ABI identity
include/   host copies of the host/Linux bridge and host-only guest contracts
platform/  Apple capabilities, VM, files, signals, sockets, and the hardware
           video encoder and decoder
memory/    guest address spaces, software MMU, and TLBs
cpu/       gadgets, JIT, executable memory, shared decode, and neutral
           engine selection
runtime/   guest-entry composition
device/    block, console, GPU, input, audio, video, vsock, and network
           providers, and the vhost-user transport they share
emulator/  options and product composition
app/macos/ standalone process adapter
app/initramfs/ host tool that writes a guest's per-run initramfs
```

Targets and source lists belong to these directories; `CMakeLists.txt` here
only composes them. Subsystem contracts are in the README beside the code:
[platform](platform/README.md), the CPU engines
([arm64 gadgets](cpu/arm64/gadgets/README.md),
[arm64 JIT](cpu/arm64/jit/README.md),
[AArch32](cpu/arm64/aarch32/README.md),
[x86_64 gadgets](cpu/x86_64/gadgets/README.md),
[x86_64 JIT](cpu/x86_64/jit/README.md)), and the devices
([audio](device/audio/README.md), [block](device/block/README.md),
[video](device/video/README.md), [vsock](device/vsock/README.md)).

## Products and defaults

The standalone macOS application supports the arm64-userspace Linux kernel and
the separate x86_64-userspace kernel, both with 4 KiB guest pages, and the
Android composition. `cmake --preset macos-x86_64` selects the x86_64 artifact
and host gadget backend; the other presets select arm64 userspace. The launcher
defaults for `mem=` and `ncpus=` are in `app/macos`; `ncpus=` follows the
[applications' vCPU limit](../docs/architecture.md#memory-and-executable-code),
and an explicit kernel argument overrides either. The x86_64 product defaults to
`cgroup.memory=nokmem`: kernel-object charging is outside that compatibility
slice, and an explicit `cgroup.memory=` overrides it.

Each macOS executable embeds an Info.plist (`app/macos/Info.plist.in`) that
names its bundle identifier, by which Metal places the process's shader cache
([Metal shader cache](../docs/graphics.md#metal-shader-cache)). Privacy
permissions are requested under the code-signing identifier `uml-darwin` and
attributed to the terminal or application that launched the emulator. Signing
the executable again with `codesign` binds the property list and makes the
bundle identifier its code-signing identifier.

The emulator library chooses no guest init and configures no guest network; the
image owns both. Each Linux product embeds the boot base archive for its
userspace architecture, and unless the command line names `init=` or `initrd=`
the launcher boots it with a per-run initramfs (an explicit `init=/bin/sh`
starts a raw shell on the image). The
[boot ramdisk contract](../tools/rootfs/boot-ramdisk/README.md) owns the
archive, its parameters and the host-side `uml-initramfs` tool.

The Android macOS composition is a separate product: it consumes a compatible
Android kernel artifact, accepts a rootfs directory and one directly writable
GPT image without comparing rootfs release identity, and never searches for the
Linux desktop image. Kernarium's runtime frameworks and defaults are in
[`../Kernarium/README.md`](../Kernarium/README.md).

## Runtime, memory, and kernel loading

Handle and run ownership, the vCPU carriers and host signal ownership, the
native kernel window, guest RAM commitment, accepted-artifact sharing, and the
signed kernel loader are owned by the architecture: see
[runtime ownership](../docs/architecture.md#runtime-ownership),
[memory and executable code](../docs/architecture.md#memory-and-executable-code),
and [CPU execution and scheduling](../docs/architecture.md#cpu-execution-and-scheduling).

## CPU engines

Engine selection, executable-memory ownership, the JIT's semantic slow path
and the x86_64 userspace profile are in the
[architecture](../docs/architecture.md#cpu-execution-and-scheduling); each
engine's lowering is in the README beside it. The cache contracts the engines
share:

- **Gadget translation cache.** A cache slot indexes a translation held in its
  address space's arena, tagged by the id the cache gives each resident address
  space, so retiring an address space drops its state as a unit. Slot geometry
  is one build-time setting for both ISAs
  (`cpu/gadget_cache_geometry.hpp`).
- **Shared bodies.** Executors running one address space share its bodies, link
  records and indirect sites unless `DARWIN_UML_GADGET_SHARED_BODIES=0`; a code
  change one vCPU publishes reaches the others at their next block transition.
  The machinery is `cpu/gadget_shared_space.hpp`, `gadget_body_arena.hpp` and
  `gadget_generation_window.hpp`.
- **Bounded growth.** A shared space is renewed once its arena is mostly freed,
  a private cache sweeps link state nothing names, an address space the kernel
  destroyed is let go of at the executor's next entry, and a process-wide byte
  budget (`cpu/gadget_cache_budget.hpp`) caps the whole. The emulator sets the
  budget before the first executor, from guest memory and the process memory
  limit or `--gadget-cache-budget=`; it is also the gadget-cache term of the
  host GPU's budget ([architecture](../docs/architecture.md#memory-and-executable-code)).
- **Code writes.** In both arm64 engines, code another vCPU changed and
  synchronized takes effect at this vCPU's next context synchronization point.
  A kernel write into a guest-code page reaches arm64 code only through the
  guest's own `IC IVAU` or `cacheflush()`, as on hardware that advertises
  neither DIC nor IDC.
- **Forwarding and diagnostics.** Scalar and vector result forwarding are
  template-owned and can be disabled per program through `BuildOptions`.
  `UML_GADGET_EXECUTION_HISTOGRAM` is a default-off diagnostic build whose
  streams measure instruction mix, never speed.
- **JIT.** Private and shared translation caches derive from one process
  budget (`cpu/arm64/jit/jit_cache_geometry.hpp`). Links live in
  executor-private writable data outside executable mappings and are scoped to
  an address space. The semantic slow path performs each atomic, exclusive and
  ordered access as one host access, so it is single-copy atomic against native
  code on other vCPUs and host threads.

## Devices

The vhost-user transport and the Apple graphics stack are in the
[architecture](../docs/architecture.md#devices-and-graphics) and
[`../docs/graphics.md`](../docs/graphics.md). The Venus route's host side fails
closed on an invalid memory type, range, ownership, acknowledgement, or
reconnect state. The devices Kernarium attaches and its display geometry are
listed in [`../Kernarium/README.md`](../Kernarium/README.md#devices-and-display).

### User network

The SLiRP provider owns the user network. libslirp comes from the maintained
series in [`../dependencies/README.md`](../dependencies/README.md), whose
patches provide the behaviors the `slirp-network-*` tests check.

- Host forwards belong to the provider and survive provider reopen; an
  unavailable host port is reported without removing the guest's network
  interface. Filesystem socket paths are removed only by their owning guards or
  the common termination cleanup.
- A forward queues up to the host's `SOMAXCONN` pending connections; the
  advertised receive window never exceeds the free buffer; a connection whose
  guest end stops answering is closed; a connect the host stack refuses is
  answered with ICMP net-unreachable (port 0 excepted); ICMP echo is forwarded
  as a router would.
- The provider answers the guest's name server itself, through the host's system
  resolver, and SLiRP's own DNS proxy is off: the proxy would send queries from
  the application to a local-network router, which needs the Local Network
  permission, and the guest follows the device's own resolver configuration.
  The wire rules are `network-dns-rules`.
- Guest IPv4 multicast is dropped on the virtual link. The Local Network
  permission is asked for only when the guest itself reaches the local network.
- The guest network driver has no transmit flow control, so a guest transmit
  into a full ring waits briefly for the SLiRP loop (`guest-slirp-network-transfer`).

## Guest storage and lifecycle

Sync, quiesce, resume, and the management console are in the
[architecture](../docs/architecture.md#guest-lifecycle). Disk capacity is an
application concern: the runtime validates that a GPT matches its file and
boots it without rewriting either, and the
[Android static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)
owns how an image and its filesystem grow. When Kernarium syncs, quiesces, and
resumes is in [`../Kernarium/README.md`](../Kernarium/README.md#lifecycle).

## API and validation

The installed API is under `api/include/uml/`; its ABI identity is independent
of the host/kernel bridge under `../kernel/common/abi`. Follow the ABI README
for declaration changes.

Use the workflows in [`../docs/testing.md`](../docs/testing.md). The
architecture audit verifies source ownership, dependency direction,
executable-memory ownership, test registration, and platform isolation. Guest,
package, graphics, and performance behavior require their separate workflows.
