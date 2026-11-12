# Documentation map

Documentation is organized by stable ownership rather than by project phase or
investigation date.

| Topic | Owner |
| --- | --- |
| Architecture, dependency direction, runtime boundaries | [`architecture.md`](architecture.md) |
| Build, test, guest, sanitizer, and package entry points | [`testing.md`](testing.md) |
| Apple graphics support and limits | [`graphics.md`](graphics.md) |
| Virtio sound: playback pacing, the null output, capture and the microphone | [`../darwin/device/audio/README.md`](../darwin/device/audio/README.md) |
| Virtio video decoding, encoding and the private wire contract | [`../darwin/device/video/README.md`](../darwin/device/video/README.md) and [`../kernel/common/video/README.md`](../kernel/common/video/README.md) |
| Platform hardware video encoder: discovery, cache, sessions, recovery | [`../darwin/platform/README.md`](../darwin/platform/README.md) |
| Virtio vsock: the host end of the guest's vsock device, for a service such as adbd | [`../darwin/device/vsock/README.md`](../darwin/device/vsock/README.md) |
| Guest VA-API backend over the video decoder and encoder | [`../tools/video/va-v4l2/README.md`](../tools/video/va-v4l2/README.md) |
| Attempted performance work | [`optimization-history.md`](optimization-history.md) |
| Performance tools, probes, and profilers | [`../tests/perf/README.md`](../tests/perf/README.md) |
| Test fixtures: the video decoder frames and the vhost-user test devices | [`../tests/support/video/README.md`](../tests/support/video/README.md) and [`../tests/support/vhost_user/README.md`](../tests/support/vhost_user/README.md) |
| Android test probes: motion input, storage accounting and the WebGL workload | [`../tests/integration/android/motion/README.md`](../tests/integration/android/motion/README.md), [`../tests/integration/android/storage/README.md`](../tests/integration/android/storage/README.md) and [`../tests/integration/android/webgl/README.md`](../tests/integration/android/webgl/README.md) |
| A64 instruction coverage | [`a64-instruction-coverage.md`](a64-instruction-coverage.md) and `tests/unit/arm64/*.tsv` |
| A64 instruction corpus | [`../tests/unit/arm64/corpus/README.md`](../tests/unit/arm64/corpus/README.md) |
| A32 instruction coverage | [`a32-instruction-coverage.md`](a32-instruction-coverage.md) and `tests/unit/arm64/a32_instruction_inventory.tsv` |
| AArch32 decoder and encoding table | [`../darwin/cpu/arm64/aarch32/README.md`](../darwin/cpu/arm64/aarch32/README.md) |
| AArch32 instruction corpus and its oracle | [`../tests/unit/arm64/aarch32_corpus/README.md`](../tests/unit/arm64/aarch32_corpus/README.md) |
| x86-64 instruction corpus, its masked-flags data and the producers of its cases | [`../tests/unit/x86_64/corpus/README.md`](../tests/unit/x86_64/corpus/README.md), [`../tests/unit/x86_64/masked_flags/README.md`](../tests/unit/x86_64/masked_flags/README.md), [`../tools/x86-tests/README.md`](../tools/x86-tests/README.md) and [`../tools/fex-tests/README.md`](../tools/fex-tests/README.md) |
| Instruction corpus sources and their generator | [`../tools/instruction-corpus/README.md`](../tools/instruction-corpus/README.md) |
| Darwin runtime details | [`../darwin/README.md`](../darwin/README.md) |
| Android guest storage on the host: layout file, partition tables, data disk, exactly two disks | [`../darwin/device/block/README.md`](../darwin/device/block/README.md) |
| Kernarium iOS application | [`../Kernarium/README.md`](../Kernarium/README.md) |
| Andarium iOS application (Android) | [`../Andarium/README.md`](../Andarium/README.md) |
| Source both iOS applications share: the runtime loader, display and input, disk and zstd utilities | [`../GuestHost/README.md`](../GuestHost/README.md) |
| arm64 gadget engine internals | [`../darwin/cpu/arm64/gadgets/README.md`](../darwin/cpu/arm64/gadgets/README.md) |
| arm64 JIT engine internals | [`../darwin/cpu/arm64/jit/README.md`](../darwin/cpu/arm64/jit/README.md) |
| x86-64 gadget engine contracts | [`../darwin/cpu/x86_64/gadgets/README.md`](../darwin/cpu/x86_64/gadgets/README.md) |
| macOS-only x86-64 JIT contracts | [`../darwin/cpu/x86_64/jit/README.md`](../darwin/cpu/x86_64/jit/README.md) |
| External dependencies | [`../dependencies/README.md`](../dependencies/README.md) |
| Kernel families | [`../kernel/README.md`](../kernel/README.md) |
| Shared kernel contracts | [`../kernel/common/README.md`](../kernel/common/README.md) |
| Mainline kernel policy, including the Android kernel product | [`../kernel/mainline-6.18/README.md`](../kernel/mainline-6.18/README.md) |
| LineageOS 24 (Android 17) producer and guest image | [`../android/lineage-24/README.md`](../android/lineage-24/README.md) |
| Linux guest images and the boot ramdisk | the README beside each producer under [`../tools/rootfs/`](../tools/rootfs/); the Vulkan guest image and its receipt: [`../tools/rootfs/vulkan/README.md`](../tools/rootfs/vulkan/README.md) |
| Public and kernel ABI maintenance | [`../kernel/common/abi/README.md`](../kernel/common/abi/README.md) |
| Licensing: the project licence, the rule by location, carried content | [`../LICENSES/README.md`](../LICENSES/README.md) |

`scripts/` holds build, packaging and patch-series infrastructure that CMake
and other tools call. `tools/` holds developer-facing launchers, producers and
generators, together with what each one owns: the guest sources, patch series
and boot files a producer builds from, and the runner for a corpus a generator
writes. CMake and CTest may read or run those files in place.

The project is licensed under GPL-2.0-or-later, with Apache-2.0 for the code
that goes into the Android image and the licence of the patched project for a
patch to another project. The root [`LICENSE`](../LICENSE) is the GPL-2.0 text
and [`../LICENSES/`](../LICENSES/README.md) holds the rest.

A document states the current contract of the subject it owns; rationale and
decisions stay out of tracked files (the documentation policy in
[`../AGENTS.md`](../AGENTS.md)). Machine-readable manifests, identities, and
test inventories are the source of truth for exact lists and counts.

The repository intentionally has no handoff, progress, evidence-index, or
dated investigation documents. Durable results belong in the owner above;
implementation history belongs in Git.
