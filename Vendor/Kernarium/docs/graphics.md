# Apple graphics support

This document owns the product's graphics boundary and claims and the
contracts of the stack that implements them. Mechanics live in the code, in
`darwin/device/gpu`, and in the maintained Mesa and virglrenderer series under
`dependencies/patches`.

## Current path

Every Apple product uses one Virtio-GPU rendering path on one host GL
provider:

```text
guest Mesa -> Virtio-GPU/Virgl -> vhost-user -> virglrenderer
    -> Mesa Zink over KosmicKrisp -> MTLTexture -> AppKit or UIKit
```

- The provider is Mesa's Zink on the packaged KosmicKrisp Vulkan driver,
  reached through Mesa's surfaceless EGL platform on an EGL device it names
  explicitly, so another EGL client of the process cannot disturb guest
  contexts. It renders a desktop OpenGL 4.6 core context, which needs
  `GL_ARB_buffer_storage`; the coherent buffers reach the guest through the
  vhost-user shared-memory aperture of the shared kernel series.
- virglrenderer and libepoxy are upstream releases carrying the maintained
  host series. The dependencies come from the verified
  `apple-gpu-vulkan-experiment` prefix. No Apple GPU product links CGL,
  OpenGL.framework, EAGL, MoltenVK, a Vulkan loader, or a second ambient
  libepoxy/virglrenderer family.
- macOS and the iOS device build the stack from the same sources. The iOS
  Simulator cannot run it, because KosmicKrisp requires Metal 4; it has the
  [software renderer](#ios-simulator-software-renderer). The unmodified device
  slice also runs on an Apple silicon Mac in Designed for iPad mode; the
  physical-device lanes are in [`testing.md`](testing.md).
- Destroying a GL context returns the Metal pipelines and generated MSL it
  caused, so closing a guest application does not keep them for the life of
  the process.

Presentation:

- The GPU transport thread owns renderer creation, context use, reset and
  teardown. Fenced Virgl submissions retire only after the renderer reports
  the host-owned timeline cookie, and commands that mutate or tear down
  renderer state wait for deferred fences. A refused command buffer or
  transfer fails alone.
- AppKit and UIKit share the Metal presentation layer. A Virgl scanout reaches
  the native display as the Metal texture Zink exported, when that texture
  holds exactly the scanout's source image; anything else takes the CPU
  readback path. Venus scanouts keep their native Metal textures. Presentation
  needs no host fence of its own: the guest flushes a scanout only after the
  fence of the rendering it shows has retired.
- VNC is a macOS consumer of the same renderer for diagnostics and remote
  display, not a second backend. Its framebuffer keeps the size it was created
  with and aspect-fits every guest mode. Readback is asynchronous and never
  blocks the guest's flush timeline, and flushes retire without readback while
  no client is connected.
- The native presenter can mirror the display to the application
  (`darwin/device/gpu/frame_tap.hpp`), as the Picture in Picture window needs.
- A guest cannot tell a presented frame from a discarded one.

Guest-visible guarantees:

- A texture the renderer creates reads as zeros until the guest writes it,
  because the host driver hands out a released resource's memory with its old
  pixels, which may have belonged to another guest process. Imported and
  compressed-format textures keep their content.
- Images shared through native Metal heaps keep tiled storage but disable
  lossless compression, so a compositor can sample a render target. A Virgl
  import of such a blob is bounded by what backs it; an import that does not
  fit is refused.
- Pointer semantics are selected by the product launcher. Android input is one
  direct touch contact with absolute coordinates; other guests keep absolute
  pointer hover, buttons, and wheel.

## iOS Simulator software renderer

The Simulator has no Metal 4 device for KosmicKrisp, so Kernarium and Andarium
render a guest there on the CPU. That is a product for UI tests, not a faster or
second graphics backend, and no other product can carry it:

```text
guest Mesa -> Virtio-GPU/Virgl -> vhost-user -> virglrenderer
    -> Mesa llvmpipe over surfaceless EGL -> readback -> UIKit
```

- Its `apple-gpu-vulkan-experiment` prefix (`ios-simulator-arm64`) holds
  libepoxy, virglrenderer built without Venus, and Mesa's llvmpipe and softpipe
  drivers; it has no KosmicKrisp, Vulkan header or Venus protocol
  ([dependencies](../dependencies/README.md)).
- llvmpipe's JIT is Mesa's own: it maps and publishes code itself and does not
  go through `darwin/cpu/jit_memory`. It is the one exception to the
  [executable-memory rule](architecture.md), granted for this test-only
  product.
- The host renders a desktop OpenGL 4.6 core context as the other products do.
  The guest sees a Virgl GPU with OpenGL ES 3.2 and no Vulkan: the device
  offers no Venus capset, refuses a Venus context, and plans no GPU budget.
  Every flush is a readback (`renderer=software` in the log).
- The rasterizers are a build-time property: `UML_GPU_SOFTWARE_RENDERER`
  derives from the `ios-simulator` platform alone. The architecture audit, the
  prefix verifier and `Kernarium/scripts/package-ios-frameworks.sh` reject the
  software renderer in any other product.

## Guest capability boundary

What the guest is told must be what the host can do.

- **Versions.** On macOS the guest reports desktop OpenGL 4.6 core and GLSL
  4.60 through Virgl, backed by the host-visible aperture the device always
  publishes to Virgl contexts. That is the profile the guest reports, not a
  conformance result: guest Zink over Venus is the qualified desktop OpenGL
  path on macOS, and the Virgl path stands on its recorded campaigns. GLES
  comes from the same capsets; macOS and an A15 iPad report OpenGL ES 3.2 with
  every extension of the Android extension pack, and the Android guest
  declares both.
- **Formats are what the host stores.** Virgl advertises a depth format only
  where the host's storage has that width and datatype, offers `S8_UINT`
  stencil on its own, and gives a scanout bit only to the 8-bit RGB layouts the
  display presents. A scanout bit on a YUV format would cost the guest every
  video encoder, decoder, and camera use of it.
- **YUV and padded buffers.** Without a host allocator a YUV buffer is an R8
  image. A guest tags the 8-bit YUV formats (`VIRGL_CAP_V2_PACKED_YUV`) so the
  renderer keeps a texture per plane; a use the plane textures cannot serve
  converts the image to a plain one with a warning. Single-plane buffers pad
  their rows to 16 bytes, the pitch Metal reads a texture over a buffer at, and
  a padded buffer is locked by one `VIRGL_CCMD_TRANSFER3D`. Emulated YUV
  layouts are the layouts Android defines.
- **ETC and ASTC.** The renderer offers ETC1, ETC2, EAC and ASTC LDR as host
  formats, which KosmicKrisp samples natively, so a guest texture in one of
  them keeps its compressed size rather than being decoded to RGBA8 by guest
  Mesa. The host's desktop context copies them by OpenGL ES's copy classes,
  as the guest's `glCopyImageSubData` expects.
- **Compressed formats on iOS.** An Apple GPU on iOS aborts the process when
  given a BC, RGTC, or BPTC texture, so KosmicKrisp asks
  `MTLDevice.supportsBCTextureCompression` and reports those formats, and
  Vulkan `textureCompressionBC`, unsupported where it says no. Every Apple
  silicon Mac supports BC.
- **Video.** Hardware video decode is not advertised over Virgl. Stateful V4L2
  codecs use the independent [Virtio video service](../darwin/device/video/README.md);
  the guest's VA-API driver
  ([`uml_drv_video.so`](../tools/video/va-v4l2/README.md)) puts the same nodes
  behind VA-API clients. H.264 High10 is advertised only where a
  profile-specific VideoToolbox probe succeeds, which excludes macOS.
- **Framebuffer completeness.** Two framebuffers that
  `KHR-GL46.direct_state_access.framebuffers_check_status` prefers incomplete
  are reported complete, as the specification requires: one mixes
  renderbuffers created with one and two samples (the frontend promotes one
  sample to two, because Gallium has no one-sample multisample surface), and
  one attaches a slice of a 3D texture beside a 2D texture, which leaves no
  attachment layered.

The boundary has two sides. Guest userspace is an external input: the Mesa,
libdrm, WSI, CTS executables and desktop packages installed in `rootfs.erofs`
are used exactly as they are, never replaced, preloaded, overlay-substituted,
rebuilt, or repackaged. The guest kernel is this project's: required
`virtio_gpu`, DRM, UAPI, Kconfig, or `arch/um` transport work belongs in the
maintained Linux series. Prefer standard Virtio-GPU and DRM interfaces; a
kernel-to-emulator extension is acceptable only when negotiated, bounded,
invisible to guest userspace, and presented through an ABI the unmodified guest
Mesa and libdrm already understand. A private userspace ioctl or library must
never become a production dependency.

One guest file is the single exception: the gbm backend module
`virtio_gpu_gbm.so`, which the Debian desktop images ship beside the video
driver ([va-v4l2](../tools/video/va-v4l2/README.md#the-gbm-module-planar-buffers-for-debians-libgbm)).
Debian's libgbm cannot allocate NV12, P010 or any other planar format, and
neither the kernel nor the host can change that. The module is not a
replacement or a preload: unmodified libgbm loads it through its documented
backend interface and it adds only planar buffers. Deleting the file, or
`GBM_BACKEND=dri` for one process, restores Mesa alone.

The GPU advertises display geometry and density through EDID; Kernarium's
point and Retina geometry is in
[`../Kernarium/README.md`](../Kernarium/README.md#devices-and-display).

## Dynamic native geometry

- Native AppKit and UIKit displays publish host geometry only after it
  settles: when live resize ends, when an interactive transition completes, or
  when the window scene reports a resize gesture over. The guest sees only the
  size a gesture settles at.
- Host-to-guest automatic resizing is on by default (an iOS app setting;
  `--gpu-auto-resize=on|off` on macOS). A settled change updates the
  Virtio-GPU advertised mode and raises one display event; a duplicate does
  not raise another.
- Every applied guest mode is authoritative for presentation, whether the
  guest chose it or rounded a host request, and whether its scanout is a
  classic resource or a blob shown through its Metal texture. macOS resizes its
  window to the applied size; iOS aspect-fits every mode in its view and VNC in
  its fixed framebuffer. With automatic resizing off, host changes leave the
  guest mode alone and aspect-fit it.
- Only the CPU path keeps host copies of a mode's pixels; they are allocated on
  first use, released on a mode change or clear, and an allocation failure
  reaches the flush that needed it as an out-of-memory response.
- Rendering and input share one centred aspect-fit rectangle. A pointer or
  touch in a letterbox bar reaches the guest only as part of a gesture begun
  inside the rectangle, and a guest mode change releases held buttons and
  contacts.

## Vulkan

Every GPU build advertises guest Vulkan except the iOS Simulator's [software
renderer](#ios-simulator-software-renderer), which has no Vulkan driver. Venus
runs over the packaged KosmicKrisp on Metal, and the guest reaches the host ring
through the kernel's vhost-user shared-memory aperture. The device of every
other build always offers the Venus capset, blob resources, and that aperture;
no runtime switch withholds them.

Venus exposes KosmicKrisp's real version, extensions, features, properties,
formats, queues, limits, and memory model, with no injected extension or forced
version, and only what runs reliably through the whole guest-to-Metal path is
advertised. No advertised capability depends on CPU shader interpretation, CPU
rasterization, resource-sized steady-state copies, or readback-driven command
decisions. Functionality Metal or KosmicKrisp cannot run reliably is reported
unsupported instead.

That route makes no conformance claim. Support is asserted only by the
receipt-bound guest CTS campaigns in
[`testing.md`](testing.md#graphics-conformance-campaigns).

### Guest memory boundary

A guest application drives this renderer, so every layer that writes through an
address derived from guest state bounds that write itself. No guest can make
the host write outside the memory it gave the host, whatever the GPU does:
only a resource's attached backing reaches guest RAM, a control-queue reply is
never written past the descriptors the guest supplied (a command whose reply
does not fit does not run), a Venus reply stream ends at the window the guest
declared, and the driver bounds a transform feedback store by the buffer range
bound for it.

### Host-visible memory is bounded by the blob aperture

Host-visible Vulkan memory never becomes guest pages. The renderer allocates it
as host shared memory and the device maps it into the guest's blob aperture,
one fixed guest-physical region of `protocol::hostVisibleSharedMemoryRegionSize`.
That constant and the kernel's `UML_VHOST_USER_SHMEM_SIZE` are one value, so
changing it is a change to the kernel as well as to the host.

The aperture, not host memory, bounds host-visible memory: the host-visible
memory type gets its own heap sized to it. A mapping near the aperture's size
can fail at `vkMapMemory` with `VK_ERROR_OUT_OF_DEVICE_MEMORY` until the guest's
released nodes retire.

### Device memory budget

Device memory is charged to the application like guest RAM, so the host gives
the GPU a fixed budget, B, computed once at launch from the process's memory
limit and the guest's memory (the formula is in [memory
architecture](architecture.md#memory-and-executable-code)). The budget is one
pool (`device/gpu/device_memory_pool.*`) for everything the host GPU holds for
the process: Metal's allocation counter less the host memory the drivers only
import, plus what the graphics ledger the system counts the process by holds
beyond the heaps KosmicKrisp made resident, which both hold. The counter holds
the IOSurfaces of presentation and decoding, which the ledger does not, and the
ledger the driver's command storage, which the counter does not, so both count
(`gpu-host-gl-surface-and-command-storage`); the host's own Metal textures and
buffers, which both hold too, count twice. The pool keeps 16 MiB of B in
reserve: it refuses a reservation that would take its usage above B - 16 MiB,
its ceiling, so what it counts but never refuses grows into that room before it
passes B (`gpu-device-memory-pool`). The renderer shrinks every advertised heap
to the ceiling and bounds every heap budget it reports by the pool's headroom,
so a guest sizes its allocations from the ceiling less what the host already
holds.

KosmicKrisp is the one place every allocation passes: every device the process
creates, Venus guests' and host GL's alike, reserves a heap from the pool before
Metal makes it, so allocations that each fit alone never together take the
process past B. The ledger counts a heap only once its pages are resident, so
the driver makes each heap resident before its reservation ends, and a burst of
creations with no work between them is refused at the ceiling
(`gpu-host-gl-allocation-burst`). A reservation that does not fit first frees
the buffers the host GL driver caches, and the command storage it has left
unused for a while, and is decided again. Memory the host frees leaves Metal's
counter at once but the ledger only later, within milliseconds or at its next
sweep. The pool sees the release in Metal's counter, and when the request would
fit once the ledger had caught up with it, waits for the ledger (re-reading it
every millisecond, for 300 ms at most) rather than refuse. A request that
follows no release, or would not fit even then, is refused at once, and a wait
that reached its bound is not repeated until Metal's counter falls again.

- `vkAllocateMemory` of any type is refused with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` when the pool cannot hold it. That covers
  every buffer and texture of the OpenGL path. Host-visible memory the renderer
  makes for the guest is reserved before its pages are made. Imported memory is
  neither charged nor counted: Metal counts it in full, so KosmicKrisp reports
  the bytes it imports and the pool leaves them out. Command, descriptor and
  query pools, pipelines, the driver's command storage, presentation, the VNC
  surface and decoder surfaces are counted and never refused, and they, with
  the geometry heap floor below, are the only way usage passes the ceiling.
- OpenGL: a refused texture or buffer makes the renderer give back what it holds
  for finished work and create the resource once more: it finishes and trims
  every context, frees the deferred shared-memory segments and the buffers the
  host drivers cache, and waits for the freed memory to count in the pool, for
  300 ms at most (`gpu-host-gl-reclaim-retry`). Taking any of that again goes
  through the pool. The drivers' command storage does not, so it goes only once
  it has been unused for about a second. A reclaim that did not help rules out
  another until a resource has been destroyed or a quarter of a second has
  passed. A resource that still does not fit fails the creation with `ENOMEM`,
  which the device answers as out of memory when the guest created the resource
  with its own command: the guest kernel fails the creation ioctl and the
  guest's GL driver reports `GL_OUT_OF_MEMORY`. The context, the renderer and
  every resource made within the budget stay as they were
  (`gpu-host-gl-device-memory-budget`). A resource created inside a command
  buffer, such as a gralloc blob's storage, loses the answer there: the
  renderer drops the rest of that buffer and remembers the refused blob id, so
  the blob that names it is answered with `ENOMEM` once, which the device
  reports as out of memory (`gpu-host-gl-refused-blob`). The guest kernel's
  creation ioctl waits for that answer, for a blob as for any other resource,
  and fails before the guest holds a handle (`-ENOMEM` for an out-of-memory
  answer, `-EINVAL` for any other error), so gralloc fails the allocation and
  no refusal leaves a buffer another process can import
  (`guest-virtgpu-blob-refusal`). A driver's own
  staging buffer that is refused inside a transfer is not retried. The buffers
  a draw uploads, the program's constants and the attributes it has no array
  for, come from the driver too: a draw whose upload is refused is skipped and
  raises `GL_OUT_OF_MEMORY`, and the next draw sets its state up again
  (`gpu-host-gl-upload-refusal`).
- Venus: the guest driver submits `vkAllocateMemory` asynchronously, so a
  refusal reaches the guest as a failed context at the next command naming the
  allocation, and only that guest process aborts. The Android guest allocates
  synchronously, so there the application gets `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  and keeps its device. A refused Venus allocation is not retried.
- A device's first geometry heap is made at its floor size even when the pool
  refuses it, so a full pool can leave the process one floor per device past
  the ceiling.

The counter includes memory the guest has freed while its blob resource is still
referenced, so a loop that reallocates large images can be refused while its
live memory is below B.

The device counts each OpenGL resource creation or blob it answers with out of
memory, and each Venus allocation the pool refuses, on the display
(`Display::memoryRefusals`), so a refusal the host reclaimed and retried past
is not counted and a restarted guest does not reset the count. The applications
read it as `gpu_memory_refusals` of the runtime's statistics, and the macOS
launchers print it as `gpu-refusals` with `--stats`. The renderer reports a
Venus refusal through the pool's `refused` callback, which the device
registers.

The same reading carries B and the pool's usage, the count the budget refuses
by (`gpu_memory_budget_bytes` and `gpu_memory_used_bytes`, runtime API 27): the
count above, plus the reservations in flight, memory the drivers keep for reuse
included. The macOS launchers print them as `gpu-memory <used>/<B> MiB`.

The display also keeps who holds the budget (`context_memory_ledger.*`): a
Virgl resource or blob is credited in full to every context it is attached to
and counted once in the total, and a Venus context's device memory comes from
the renderer's per-context report (the pool's `account` callback). The guest
kernel names a context's creating process in the context's debug name
(`<name>|pid=<tgid>|comm=<comm>`, the
[kernel contract](../kernel/common/README.md#devices)); a kernel that does not
leaves the holders unnamed and nothing acts on them. `read_graphics_memory`
(runtime API 28) lists the quiet holders by process, and the macOS launchers
print them with `--stats`. Andarium ends the cached background app that holds
the most when the budget is nearly full (`GraphicsReclaim`).

The guest's own low-memory killer can do that instead. The device offers a
private Virtio-GPU extension (`VIRTIO_GPU_F_UML_MEMORY_REPORT`, bit 23, only
while the renderer has a budget; the wire contract is
[`kernel/common/gpu`](../kernel/common/gpu/README.md)): one query command
answers with the budget, the pool's usage, what the contexts hold, the refusal
count and up to 16 holders by process, and the guest kernel serves the answer
as `gpu_memory_report` ([kernel contract](../kernel/common/README.md#devices),
`guest-virtgpu-memory-report`). The answer comes from the ledger and the pool
on the transport thread and never waits for the renderer. A query that
subscribes also makes the device raise a config-change event (`events_read`
bit 1) when the pool goes under or out of pressure, and at most once a second
when a refusal was counted; the guest then asks again.

- Pressure is `usage + 96 MiB >= B - 16 MiB` on the pool's own usage, the
  figure a reservation is refused by, so it means "within the margin of what
  the pool refuses at". The usage holds memory the host frees itself, so when
  the condition first holds the transport runs the pool's reclaim hook, the one
  that runs before a refusal (at most once a second, from the transport
  thread), the figure is given 300 ms to show what it freed, and pressure is
  declared only if the condition still holds after that and has held for 1 s.
  It is cleared after 5 s without the condition
  (`device/gpu/memory_pressure.*`, the evaluator with an injected clock). A
  guest allocation that arrives faster than the margin fills in that second is
  still refused first, which the refusal edge reports.
- Nothing is periodic and nothing wakes an idle device: the ledger's change
  observer arms a coalescing 250 ms timer on the transport thread, a refusal
  moves that timer to the next millisecond, and the evaluator arms one
  deadline for its own next change.
- A query that sets `GUEST_RECLAIMS` records that a reclaimer in the guest is
  armed. The flag belongs to the connection, is cleared by a query without it
  and by a new connection, and reaches the applications as `guest_reclaims` of
  `read_graphics_memory` (runtime API 29) and the launchers' `--stats` line as
  `guest-reclaim=on`. While it is set `GraphicsReclaim` stands down and does not
  end an app.

### Android guest requirements

The Android guest reaches the same renderer by a separate route: one Soong port
of this project's guest Mesa provides both the Virgl gallium driver behind the
`kernarium` EGL and GLES libraries and Mesa's Venus driver as the Vulkan HAL
`vulkan.virtio.so`. That contract, including the guest epoch the
hardware-buffer path needs, why no Vulkan dEQP level is declared, and the
OpenGL ES 3.2 claim, lives in
[`../android/lineage-24/README.md`](../android/lineage-24/README.md#graphics).

The Linux Venus guest meets the API version, limits, formats and capability
groups of Khronos's `VP_ANDROID_17_requirements` profile except for three
standing limits of this stack:

- `protectedMemory` has no Metal equivalent.
- `shaderBufferInt64Atomics` needs 64-bit buffer atomics this Metal feature
  set does not provide.
- `shaderFloat16` is withheld on purpose, because native half arithmetic can
  time out Apple GPUs when NaN-heavy graphics shaders run concurrently. The
  16-bit storage classes are supported.

The renderer owns the window-system prerequisites.
`VK_EXT_image_drm_format_modifier` offers `DRM_FORMAT_MOD_LINEAR` only, because
Metal describes no other tiling an outside agent could agree on; a swapchain
whose surface offers modifiers is a linear image, and one that offers none
renders optimally and reaches the window system through common WSI's linear
prime blit. The guest's `VK_ANDROID_native_buffer` and
`VK_ANDROID_external_memory_android_hardware_buffer` conditions are met. The
loader-level window-system extensions of the profile are the Android Vulkan
loader's work, not an ICD's.

The guest driver and the renderer generate their halves of the encoder from one
pinned venus-protocol and its maintained series
([`../dependencies/README.md`](../dependencies/README.md)), so the two change
together, and an extension the protocol does not name is refused by the guest.

### KosmicKrisp driver contracts

These are the run-time guarantees of the host KosmicKrisp driver as this
project's Mesa series patches it; the series and its regression suites are in
[`../dependencies/README.md`](../dependencies/README.md).

- The driver never hands Metal a texture view it cannot make, because Metal
  answers one with an assertion that ends the process. Such a view fails with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`; a 3D view of a non-3D image is made as the
  2D or 2D-array texture the image is. A 2D view of a 3D image renders through
  the image's own texture and is sampled through a 2D-array alias; a level too
  deep for the alias is made for rendering alone.
- A point is clipped against user clip and cull distances and the depth planes,
  never the view-volume side planes (`VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY`).
- Unsatisfied timeline waits are gated on the CPU before native Metal
  submission, so delayed host signals do not outlive Metal's finite GPU
  event-wait window. `vkQueueSubmit` never waits for the GPU work its batch
  depends on. A timeline wait whose signal is submitted later to the same queue
  stays unsupported.
- Subgroup operations reach Metal's SIMD groups, so compute and fragment are
  the advertised subgroup stages; other stages present a single-invocation
  subgroup.
- An image atomic is complete before the invocation's next image or global
  access, barrier, or termination, because Metal issues an atomic whose result
  is unused without waiting for it.
- A vertex or tessellation evaluation shader that writes memory and contains a
  loop runs as compute, because Apple GPUs kill a long vertex invocation along
  with the in-flight work of every process on the GPU.
- A buffer load never covers more than sixteen bytes, because Apple's backend
  compiler can crash on a wider vector.

### Apple8 (A15) device behaviour

The physical iOS device the Vulkan route has been exercised on is an A15 iPad
(Metal family Apple8). It differs from the Apple7 Macs in ways that change
results, not only speed; the driver takes the capabilities that differ
(`supportsBCTextureCompression`, `supports32BitFloatFiltering`) from the device.

- **A GPU fault is reported as a timeout.** Metal 4 cannot tell a fault from a
  timeout, so "timeout" on this device is a label, not a diagnosis, and must
  not be read as a shader that ran too long.
- **A faulted command queue never works again.** The driver retires a queue a
  failure has killed and commits to a replacement, so a fault costs only its own
  submission and the device is not lost. A storm of timeout reports across cheap
  tests means a dead queue is still in use.
- **A killed submission takes its signals and query results with it.** The
  driver records every signal against the submission it sits behind and
  completes what a failure destroyed, and the renderer bounds a stuck fence, so
  no loss can wedge a guest forever.
- **A rejecting queue drops a bare signal in silence.** A waiter on an overdue
  signal probes the queue, and a queue that rejects the probe is retired.
- **Some GPU work faults the device, and the driver cannot remove it.**
  Enough fragment-stage store work, render-stage 3D or array texture sampling,
  or a long run of small render passes kills a submission. Conformance cases
  stand on these limits, and the mitigations that were tried are in
  [`optimization-history.md`](optimization-history.md).
- **An out-of-bounds texture atomic faults the GPU.** Metal's robust access
  covers texture reads and writes but not atomics, so the driver rejects the
  coordinate before the atomic issues.

#### Memory budget

iOS ends the app at its footprint limit, and a 1536 MiB guest already leaves the
host side little headroom, so guest RAM cannot be raised to make room for a
workload. The per-device geometry heap starts small and grows within the pool, a
grown heap is freed once nothing holds it, Zink's buffer cache is bounded, a
destroyed context's command storage is released with it
(`gpu-host-gl-batch-storage`) and an idle Virgl context is trimmed. virglrenderer's shared-memory segments are
unlinked container files outside the footprint on iOS and POSIX shared memory
inside it on macOS. A baked device campaign leaves out the groups its guest
memory cannot hold ([`testing.md`](testing.md)).

## Metal shader cache

Metal keeps one shader cache per process identity, and its first compile reads
the whole index into memory; the public API cannot clear, bound or bypass it,
and conformance campaigns fill it. Kernarium, Andarium and the macOS emulators
therefore remove the cache at start, before anything creates a Metal device,
when its index exceeds a small limit
(`GuestHost/Sources/MetalShaderCache.swift`,
`darwin/device/gpu/metal_shader_cache.hpp`). The emulators carry the bundle
identifier of their embedded Info.plist
([Darwin README](../darwin/README.md#products-and-defaults)); a process with no
bundle identifier shares Metal's cache with every other such process and is not
bounded, so the GPU unit tests keep their own cache.

## Mesa shader cache

Each application and emulator gives Mesa's disk cache a home of its own and a
size limit, once, at process start and before any thread can read the
environment (`GuestHost/Sources/MesaShaderCache.swift`,
`darwin/device/gpu/mesa_shader_cache.hpp`; the two change together). A variable
already in the environment is kept, and a process with no cache directory runs
without a cache. No behavior depends on a cache entry surviving.

The cache is Mesa's one-file-per-entry format and holds no file lock: a writer
claims an entry by creating its temporary file exclusively, and a temporary
file older than an hour is a dead writer's and is replaced. iOS ends a process
it suspends while it holds a file lock, and a cache write can be in flight at
the suspension. The database and single-file formats lock their files and stay
off.

## Validation

Use `check-gpu` for native GPU ownership and VNC checks and
`check-gpu-asan-ubsan` for the sanitized path. Guest conformance, WebGL,
Aquarium, and physical-device campaigns are opt-in; see
[`testing.md`](testing.md).

Historical backend and performance experiments are summarized in
[`optimization-history.md`](optimization-history.md).
