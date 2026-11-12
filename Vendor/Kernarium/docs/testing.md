# Testing and preparation

CMake/Ninja and CTest are the first-party source of truth. Start with the
narrowest relevant target or exact test, then run the one workflow that owns
the changed behavior. `cmake --list-presets=all` lists the preset surface.

## Test suites

Every first-party CTest has exactly one primary suite, set by the registering
helper in `cmake/UmlTesting.cmake`; `ctest -N -L <suite>` lists a tree's
registrations:

- `unit`: native C++/assembly behavior; no runtime Python, shell, kernel,
  network, CLI, or rootfs dependency;
- `tooling`: repository, build, manifest, and source-policy checks; and
- `integration`: guest, rootfs, GUI, network, and kernel behavior.

Performance and long external campaigns are not CTest correctness suites.

Rules every test follows:

- Every test has a hard `TIMEOUT` (a unit test without one gets 600 s). A
  tooling test's bound is sized from its duration at gate concurrency on a
  loaded host, not alone. A wait inside a test for something the test started
  is bounded below the test's own bound and is never retried or unbounded.
- CTest ends a timed-out test with SIGINT, then SIGKILL. A Python test keeps
  its scratch in a `with` block or a `tempfile.TemporaryDirectory`, because
  `unittest` runs no cleanup callback after `KeyboardInterrupt`.
- The registration helper adds a `<suite>-build-<tool>` fixture for each built
  tool (`$<TARGET_FILE:...>`) a test runs, so running a test by name after
  configure is enough.
- Tooling tests run tools against fixtures. They do not grep tracked source for
  expected tokens or restate values another file owns. Guest shell that a test
  runs on the host gets stubs for commands with host-wide effect, such as
  `sync`.
- Unit tests create scratch files through `tests/unit/scratch/` and remove
  them, never at a host-global path unless that path is the contract under
  test. Unix-domain socket paths use `$TMPDIR` (a build-tree path may not fit
  `sun_path`). A shell tooling test's `$TMPDIR` is its `test-scratch/<test>`.
- A unit test that reads data takes its path as a registration argument, not a
  compiled-in source or build path.
- Unit tests assert with the helpers in `tests/unit/require/require.hpp`, which
  abort at the caller's `file:line`; the same directory holds `waitUntil`,
  `Watchdog` and the other shared helpers.
- CPU instruction assets are generated in the build tree from checked-in
  tables and independent golden expectations, with Python's standard library
  only. The [source contract](../tools/instruction-corpus/README.md) owns their
  format and promotion; the binary assets are not tracked.

`repository-environment-portability` is the one tooling test that scans
tracked and new unignored files, and it looks for forbidden values: host paths
and account names, private addresses and ssh endpoints, a `Claude-Session:`
trailer, and Apple signing identifiers. A site adds its private tokens to a
local denylist (`UML_PORTABILITY_DENYLIST` or
`${XDG_CONFIG_HOME:-$HOME/.config}/linux-um-darwin/portability-denylist`). A
reviewed exception goes in `tests/tooling/cmake/environment-portability-allow.tsv`,
and an entry that matches nothing fails. `check_environment_portability.py
--list-rules` describes the rules.

`repository-license-headers` enforces the [licensing rule](../LICENSES/README.md)
by location; exceptions go in `tests/tooling/cmake/license-header-exceptions.tsv`
and `check_license_headers.py --list-rules` describes the rule.

For one test, use an anchored expression:

```sh
ctest --test-dir .build/presets/macos-unit \
  -R '^exact-test-name$' --output-on-failure
```

## Canonical host workflows

Every workflow needs the
[host prerequisites](../dependencies/README.md#host-prerequisites). A test that
boots ten vCPUs (`guest-mmu-stress`) skips on a smaller host. A guest test
states its vCPU count with `ncpus=`; the launchers' default follows the
[applications' vCPU limit](architecture.md#memory-and-executable-code).

```sh
cmake --workflow --preset check-unit        # the ordinary host check
cmake --workflow --preset check-tooling     # tooling tests alone
cmake --workflow --preset check-integration # see below
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
cmake --workflow --preset check-unit-census # after a gadget cache or census change
```

The full sanitizer suites are separate (`macos-unit-asan-ubsan` with
`unit-asan-ubsan`, `macos-unit-tsan` with `unit-tsan`). Every sanitizer preset
instruments Objective-C++ too. Both sanitizers keep the product's macOS native
window, so a sanitized test takes a window both leave free
(`tests/unit/memory/native_window_test_support.hpp`), and AddressSanitizer
ignores `mlock()`, so cases that need wired guest RAM report themselves
unsupported there.

The `unit-gpu` test preset's name filter is the one selection of GPU workflow
tests: every registered `gpu-*` test plus the Virtio protocol, video and
platform-video tests the GPU configuration also builds. `uml_add_gpu_unit_test`
refuses a name that does not start with `gpu-`, configure derives
`gpu-unit-test-binaries` from the filter, and a test preset that inherits
`unit-gpu` must not set its own. A GPU unit test keeps Metal's shader cache in
its scratch directory, so a host that has run guest GPU workloads does not slow
it. The GPU workflows need the `apple-gpu-vulkan-experiment` dependency prefix;
a change across the Venus or KosmicKrisp boundary also runs
`check-integration`.

### Opt-in lanes

Each lane registers only when its input is named; the runner's `--help` or the
test's header holds the mechanics.

| Lane | Entry point | Checks |
| --- | --- | --- |
| `dependency-kosmickrisp-spirv-regeneration` | `cmake --preset macos-tooling -DUML_KOSMICKRISP_SPIRV_BUILD_HOST=<host>` | the prebuilt KosmicKrisp SPIR-V against a rebuild on the pinned Debian host |
| `apple-ios-mac-vk-gl-cts` | `macos-integration` with `UML_IOS_MAC_CTS_ROOTFS`, `_ROOTFS_RECEIPT` and `_CAMPAIGN` | a CTS campaign in the unmodified iOS device build on an Apple silicon Mac |
| `apple-ios-device-vk-cts` | `macos-integration` with `UML_IOS_DEVICE_VK_CTS_DEVICE` and `_CAMPAIGN` | a Vulkan CTS campaign on a connected iOS device |
| `guest-v4l2-decode`, `-gpu` | `UML_V4L2_PROBE_BINARY` | the [Virtio video decoder](../darwin/device/video/README.md) |
| `guest-v4l2-encode` | `UML_V4L2_ENCODE_PROBE_BINARY` | the Virtio video encoder |
| `guest-v4l2-mmap-capture` | `UML_V4L2_MMAP_CAPTURE_PROBE_BINARY` | blob-backed capture buffers allocate on an idle guest |
| `guest-futex-atomic` | `UML_FUTEX_PROBE_BINARY` | the futex contract in the [common kernel README](../kernel/common/README.md) |
| `guest-gbm-dmabuf` | `UML_GBM_DMABUF_PROBE_BINARY` | the virtio-gpu dma-buf contract |
| `guest-drm-vblank` | `UML_DRM_FLIP_PROBE_BINARY` | Virtio-GPU's vblank pacing |
| `guest-aarch32-kernel` | built from `tests/integration/aarch32/` | the kernel's AArch32 task support (needs `python3`, `as`, `ld` in the rootfs) |
| `apple-audio-playback`, `-interruption`, `guest-virtio-sound-delay-host` | `UML_TEST_APPLE_AUDIO=ON` | the real macOS audio output, the iOS interruption behavior (which needs an iOS Simulator runtime, and runs in a simulator device it creates and deletes) and a guest's stream delay on the real output |

The probe sources are built for the guest architecture; each header says how.
The real microphone is checked by hand because granting access is the user's
act ([audio README](../darwin/device/audio/README.md)).

The iOS GPU lane runs the unmodified iOS device build on an Apple silicon Mac
in Designed for iPad mode. The iOS Simulator renders on the CPU
([`graphics.md`](graphics.md#ios-simulator-software-renderer)), which suits UI
tests and no conformance or performance campaign. Only the automation
application boots a payload
([Kernarium](../Kernarium/README.md#automation-runs)), so package its runtimes
with `package-ios-frameworks.sh --automation` before either CTS lane.

A campaign is judged by its `summary.txt`, the guest's own verdict, never by a
console marker alone. A baked device campaign is the only lane whose guest
memory the platform fixes, so it is the only one that leaves cases out
(`DEVICE_EXCLUSIONS` in `run_vk_cts.py`); an excluded case is neither a pass
nor a failure, and the Mac lanes run the released selection unchanged. Host and
renderer diagnostics share the device console with the artifact frames, so a
condition on a per-frame or per-bind path reports once.

`runtime-restart-*` covers a guest that restarts itself in the same process: at
each of 64 hand-backs nothing the test kernel left behind may remain, and the
process must be as it was before the first run.

## Integration prerequisites and workflows

Integration needs four local inputs: the accepted kernel bundle below
`.build/kernel/artifacts`, the accepted boot base archive below `.build/rootfs`
([boot ramdisk](../tools/rootfs/boot-ramdisk/README.md)), the verified
dependency prefix, and the unified `rootfs.erofs` image. It needs no kernel
checkout or compiler.

```sh
cmake --workflow --preset check-integration
```

The workflow configures, builds, runs the tooling checks and executes the fast
integration suite; `integration-full` is the CTest preset for the complete
guest set. `ctest -N -L integration` lists the guest tests, and each runner or
probe states in its header what it checks. A contract's owning README, such as
[`../kernel/common/README.md`](../kernel/common/README.md), names the tests
that check it.

The serial harness boots each guest through the launcher's per-run initramfs
and the [harness session](../tools/rootfs/boot-ramdisk/README.md#harness-session).
The emulator stays in the harness's session and process group, so a signal to
the group (a timeout, Ctrl-C, CTest) reaches the guest, and a watchdog in the
group kills the guest if the harness dies alone. A behavior one program checks
inside the guest runs through `tests/integration/support/run_guest_probe.py`.

A local override of the kernel selects one whole bundle of the matching profile
through `UML_MAINLINE_618_KERNEL_ARTIFACT_DIR`; never override individual
files.

## Rootfs contract

`rootfs.erofs` is a unified immutable guest image; the per-run `/init` mounts
it read-only beneath a writable layer, and no runner names the root device or
uses it as scratch. Ordinary test guest state lives in a disposable OverlayFS
layer; the Steam appliance owns a separate persistent GPT data disk. A rootfs
update replaces the complete authenticated image through
`tools/rootfs/provision.py`; there is no in-place maintenance workflow.

The image owns accounts, credentials, login policy, gettys, desktop packages
and payloads under `/opt`; launchers must not invent replacement users or a
general login policy, and a compatibility overlay a launcher owns stays
confined to its image and session. `tools/rootfs/starter` separately owns the
writable starter disks and boot base archives ([README](../tools/rootfs/starter/README.md)).

The [x86-64 smoke image](../tools/rootfs/x86_64-smoke/README.md) qualifies the
x86_64-userspace kernel slice. That slice registers no CTest suites: its
runners (`tests/integration/x86_64/run_guest_x86_64_*.py`, `run_v4l2_*.py`,
`run_gpu_guest.py`) run directly with the `macos-x86_64` `uml-darwin` and an
x86-64 EROFS image such as the [x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md),
and the `x86-64-guest-runners` tooling test checks their options without a
guest. The runners take `--engine jit` for the macOS-only x86-64 JIT and
`UML_TEST_JIT_TIER=N` for the
[tiered engine](../darwin/cpu/x86_64/jit/README.md#tiered-execution); a change
that reaches either engine runs both. The
[Steam producer](../tools/rootfs/steam-x86_64/README.md) owns the gaming image
and its audit.

## Apple builds and Kernarium

```sh
cmake --preset ios-device && cmake --build --preset ios-device
cmake --preset ios-simulator && cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

Kernarium has runtime presets for Linux and Android on the iOS device
(`kernarium-ios-device*`) and the iOS Simulator (`kernarium-ios-simulator*`).
`Kernarium/scripts/package-ios-frameworks.sh` packages the selected runtime and
graphics slices for Xcode (`--sdk device|simulator`). No Simulator product is
built for x86_64, so a generic Simulator destination needs
`EXCLUDED_ARCHS=x86_64` on the `xcodebuild` command line. KosmicKrisp has a
device slice only; the app targets embed it with
`Kernarium/scripts/embed-device-framework.sh`. Both projects sign with the team
in the ignored `GuestHost/Config/Signing.local.xcconfig`
([setup](../GuestHost/README.md#signing)); without it a Simulator build works
unsigned.

Apple builds validate the kernel bridge ABI, page profile, product family and
`6.18` series of an accepted kernel bundle. They never prepare sources or
invoke a kernel or Android build, and rootfs bundles are launch inputs.
Kernarium's bundles and scripts are in
[`../Kernarium/README.md`](../Kernarium/README.md); its launch defaults are
`EmulatorConfiguration.swift`'s.

`Kernarium/scripts/run-ios-mac.sh` and `Kernarium/scripts/make-guest-bundle.py`
launch and assemble a bundle on an Apple silicon Mac (`--help`). Xcode's local
install service, driven by `xcodebuild test`, is the only command-line launcher
for this mode, and it passes each `TEST_RUNNER_`-prefixed variable to the app
without the prefix. An iOS application reads nothing outside its container, so
the launcher copies the rootfs into it. Derived data for a Mac run of either
application is never in a system temporary directory (`/tmp`, `/var/tmp`,
`$TMPDIR`): Gatekeeper rejects the installed app there ("bad app wrapper") and
the run never starts, so the first build phase of each application's target
(`scripts/check-run-location.py`, `apple-run-location`) fails a Mac build below
one. The shared schemes gather no code coverage; a coverage run passes
`-enableCodeCoverage YES`.

On a connected iOS device, Android runs go through Andarium
(`tests/perf/run_ios_device_android.py`, [`Andarium/README.md`](../Andarium/README.md#automation))
and Kernarium's device runners keep Linux; both share
`tests/integration/support/ios_device.py`, and their tooling tests run against
fakes, so no device is needed. Android recovery qualification uses a fresh
disposable bundle with `TEST_RUNNER_KERNARIUM_ANDROID_BOOT_MODE=recovery`;
never run wipe checks against an existing user bundle.

Kernarium's opt-in `SpacePauseGuestTests` pauses the application's own Linux
or Android guest through a free-space number, then resumes and shuts it down
(`TEST_RUNNER_KERNARIUM_SPACE_PAUSE_TESTS=1` and
`TEST_RUNNER_KERNARIUM_SPACE_HARNESS=1`; the launch selects the guest as for
any run, and the test ends it).

`KernariumTests` runs on an iPad, in the iOS Simulator
(`Kernarium/scripts/run-ios-simulator-tests.sh`, for UI tests) and, as an iPad
app, on an Apple silicon Mac. The iPad is the reference and passes everything.
On the Mac up to four tests fail because of how the Mac hosts an iPad app:
`theKeyBarNeverCoversTheConsole` and `anUnfocusedConsoleKeepsEveryRow` (the
window ends above the screen bottom), and `theKeyboardButtonFocusesTheViewOnScreen`
and `anEndedRunIsNeverStartedAgain` (SwiftUI exposes its controls only while
application accessibility is on; the latter also needs an image to boot). In
the Simulator `HostVideoEncoderTests` skip and `HostGraphicsStackTests` states
the software renderer's contract. `KernariumAutomation` is the iPad's
automation host.

`kernarium-device-framework-symbols` and `kernarium-device-framework-embed`
(tooling) cover the device-only embed script.

### Andarium

`guest-host-core` (unit, macOS) runs the Swift package `GuestHost/Core`, the
free-space thresholds, suspension check and pause both applications share, and
`andarium-core` (unit, macOS) runs the Swift package `Andarium/AndariumCore`
with `swift test`; `andarium-motion-protocol` (unit), `andarium-stage-system-image`
and `andarium-capture-screens` (tooling) cover the motion wire format, system
image staging and the screenshot script. The application builds with Xcode
(`AndariumPreview` is the Simulator build with mock data, `Andarium` the device
application, [targets](../Andarium/README.md#targets)); `AndariumTests` is
hosted in `Andarium` and runs on an iPad, in the Simulator and as an iPad app on
an Apple silicon Mac (`xcodebuild test -scheme Andarium`). The hosted tests
share the application's one key window: a test that replaces its root view
controller or connects a window takes it with `HostWindow.take()`, and none
opens the system's Settings or another app. The bundle's principal class
(`AndariumTestBundle`) shows the side keys' tip in every session of the run,
whatever the device's tip store says, except in an `ANDARIUM_HOLD_SECONDS`
harness launch, which hides it.

The opt-in suites each need a `TEST_RUNNER_ANDARIUM_*` switch that the suite's
header names: rotation, motion compatibility and over-vsock, shutdown flow,
low-space pause (a real guest paused through a free-space number the test
sets, its CPU time standing still), the graphics-memory notice (a real refusal
in Chrome's WebGL under the host's GPU budget, on a guest with Chrome
installed), the graphics-memory reading and the process list the reclaim
parses (any guest), Retina memory, the ADB-over-vsock guest
tests (including Android storage and the touch-cancel path), the side keys'
pass-through of touches to Android, the ADB-over-network hold and the
background disk locks (which also requires a guest that was allocating
buffers when the GPU was held to suspend). The storage test signs its helper
APK with the AOSP test key, never a release key. When derived data is on a
removable volume, macOS prompts once for access and the launch hangs until
answered; build to the internal disk instead.

Tooling tests cover `scripts/check-privacy-manifests.py`
(`ios-privacy-manifests`, which `package-ios-frameworks.sh` also runs on every
device slice it packages), `Andarium/scripts/check-app-store-bundle.py`
(`andarium-app-store-bundle`, the last build phase of the AppStore
configuration, [configurations](../Andarium/README.md#configurations)) and the
Acknowledgements resource (`andarium-acknowledgements`).

### UI automation on a device

Unattended device automation goes through the long-lived `KernariumAutomation`
UI-test host, because each new XCTest session on a device with a passcode needs
the owner's approval. `Kernarium/scripts/ipad-automation.py --help` has its
commands and failure modes. Keep all automation code in the test targets.

### Edge gestures (device only)

The Simulator shows which edges a run defers (`AndariumPreview -screen
session`) but not what iPadOS does with the swipes. Check on an iPad, under a
lease with the automation host and a guest using 3-button navigation, whether
the host's synthesized edge swipes start system gestures and what iOS does for
each deferred edge. A run keeps the home indicator visible, which the bottom
edge's deferral needs.

### Window layout (device only)

Window layout follows iPadOS windowing, which only a device has. Whenever the
guest view's layout changes, check on an iPad in Full Screen Apps, Windowed
Apps and Stage Manager, switching Multitasking under the running application in
both directions: the view must move at once, resize the guest display
(`wm size`), keep touch coordinates mapped and show no lost GPU backend.
Restore Full Screen Apps before releasing the lease.

### Background lifecycle (device only)

Only a device revokes the GPU, freezes the process and reclaims it. Whenever
the lifecycle handlers, the GPU hold or the quiesce change, background the
application on an iPad for a KDE and an Android guest (about 20 s and two
minutes) and read its console (`xcrun devicectl device process launch
--console`). Each cycle must show the GPU requests held and the guest's
`PM: suspend entry`, then on return the release, `PM: suspend exit`, the display
rendering, and no `virtio_gpu` response errors, Metal errors, RCU stalls or
watchdog lines; on a device it must also show guest RAM hibernated and
restored. A hold count of `0` means a request was still running after a second:
a finding, not a pass. The low-space pause adds a cycle: pause the guest
(on a device no hibernation image appears in the temporary directory, and the
guest's processor time stands still), send the application to the background
and back, then Resume; the guest must stay suspended and resident until
Resume, then wake as above. The unit tests
(`runtime-thread-fence`, `runtime-guest-hibernation`, `emulator-lifecycle`, `memory-guest-ram-image`)
cover the order on any host, and `--guest-hibernation=on` forces the same code
on a Mac. The Picture in Picture and keep-running-job modes
(`KERNARIUM_BACKGROUND_MODE`) and the restart notice after a terminated
background process are checked the same way; remove the guest bundles and
payloads such a check copied to the application's Documents afterwards.

### Guest statistics

The host side of Andarium's live statistics is covered on any host by one unit
test per source (`runtime-guest-statistics`, `guest-console-memory`,
`gpu-display-statistics`, `emulator-statistics`, `emulator-lifecycle`,
`kernarium-statistics`, `emulator-statistics-report`) and the package
consumers. `guest-statistics` (integration, GPU) boots a Linux guest with
`--stats` and checks the CPU, frame and memory figures against the guest's own;
an Android check runs `uml-darwin-android17 --stats=1000` under enforcing
SELinux and requires the memory part with no `avc` denial.

## LTP

The full LTP campaign is opt-in and runs only on the gadget or JIT engines.
`tests/integration/ltp/verified-tests.tsv` and `disabled-tests.tsv` are the
exact manifests; a disabled case is an intentional product or kernel-policy
exclusion, and a kernel feature is never enabled solely to raise the count.
The runner uses `/opt/ltp` from `rootfs.erofs` and the kernel bundle's
`modules.builtin` (the shipped kernels have `CONFIG_MODULES=n`). Case bounds
are sized on the gadget engine and scaled for `--engine jit`, which compiles
every exec'd process from scratch
([cold code](../darwin/cpu/arm64/jit/README.md#cold-code)); the factor is
recorded in the results. `--guest-arch x86_64` runs the manifests on the
[x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md) with the overrides in
`tests/integration/ltp/x86_64/`, which exist only where that userspace or the
x86 ABI changes a case's outcome. `python3 tests/integration/ltp/run_guest_ltp.py
--help` lists the options.

## CPU instruction tests

Instruction semantics belong to the corpora, each owned by its README:
[A64](../tests/unit/arm64/corpus/README.md) (inventory in
[`a64-instruction-coverage.md`](a64-instruction-coverage.md), representative
cases rather than exhaustive variant proof),
[AArch32](../tests/unit/arm64/aarch32_corpus/README.md) and
[x86-64](../tests/unit/x86_64/corpus/README.md). Engine behavior the corpus
cannot hold has focused tests whose headers say what they check
(`ctest -N -R '^arm64-(a32-|gadget-a32-|jit-a32-)'` lists the AArch32 ones; the
AArch32 JIT tests exist only on macOS). A guest `SIGILL` is a correctness
blocker in every one of them.

- `arm64-instruction-corpus` runs the generated A64 corpus through both
  engines; the special records (native SME, absent features, the SM = 0 family,
  top-byte ignore, native floating-point traps) have their own
  `arm64-instruction-corpus-*` and `arm64-absent-*` tests that re-derive them
  from their generators, and records that need SME or FEAT_AFP/BF16 run only on
  a host that has them.
- `arm64-emitted-registers` requires that no host code names x18, which Darwin
  clears when it interrupts a thread.
- `arm64-instruction-corpus-invalid-encoding` requires every
  illegal-instruction case to carry the invalid-encoding bit that
  `tools/arm64-tests/invalid_encoding.py` decides; its `-llvm` sibling is
  opt-in with the pinned LLVM MC.
- `arm64-aarch32-decoder`, `arm64-a32-encoding-table`, `arm64-aarch32-corpus`,
  `arm64-a32-corpus` and `arm64-aarch32-semantics*` check the AArch32 decoder,
  generated tables, corpus and semantic library without an engine. The
  corpus runs of `arm64-gadget-a32-corpus` and `arm64-jit-a32-corpus` retry a
  case that differs only by a spurious store-exclusive failure, and the
  `-interrupted` tests provoke that failure on purpose.
- `guest-aarch32-engine-gadgets` and `-jit` boot the freestanding engine probe
  in `tests/integration/aarch32/engine_probe/` and count every `SIGILL`.

To change the A64 oracle, its generators or the pinned Arm release, configure
with `-DUML_A64_ARCHITECTURE_ARCHIVE=<archive>` and run the opt-in
`arm64-instruction-oracle-freshness` and `arm64-sme-encoding-table-freshness`
tests. The AArch32 equivalent is `-DUML_A32_ARCHITECTURE_ARCHIVE=<archive>`
with `tests/tooling/arm64/generate_a32_encoding_table.py` and the
`arm64-a32-encoding-table-freshness` and `arm64-a32-corpus-plan-freshness`
tests. All skip without the archive or the pinned LLVM MC.

The guest-SME policy is on by default and `DARWIN_UML_GUEST_SME=0` turns it
off; the unit suites hold either way on every host. On an SME host (an M4 Mac)
also run the `arm64-sme-*`, `arm64-jit-sme-*` and corpus tests both ways.
`guest-sme-gadgets`/`-jit` and `guest-sme-off-gadgets`/`-jit` check the guest
kernel's SME contract, and `guest-no-sve-gadgets`/`-jit` that no SVE appears
outside streaming mode. The opt-in `tools/arm64-tests/sme-sm0-scan.py` and
`sme-idiom-corpus.py` list what shipped code executes outside streaming mode.

A gadget that runs an instruction as the host's own (FEAT_ECV, FEAT_I8MM,
FEAT_BF16, FEAT_FHM on an A15 iPad; FEAT_AFP and FEAT_RPRES on an A17 Pro or an
M4 Mac) runs that path only on a host with the feature. Check a change to such
a path by hand on an iOS device that has it: launch Kernarium with the
`guest-hwcap-sanity` payload and the touched families' corpus cases built into
one static guest program, and require the results of a gadget-engine run on a
Mac. The device runs only the gadget engine, so a JIT path needing one of these
features runs only on an M4 or newer Mac.

## Android

Build-side contracts need no Android build: `lineage24-target-files-fixtures`,
`lineage24-builder-lane`, `lineage24-release-contract`,
`lineage24-first-boot-precompile` and the `lineage24-mesa-venus-*` tests run
the producer's verifiers over fixtures, and `android-partition-boot-selection`,
`android17-instance-rules` and `android17-macos-launcher-options` cover the host
library and launcher over the storage layout
([storage](../android/lineage-24/README.md#static-disk-and-boot-contract),
[block device](../darwin/device/block/README.md)). `guest-android-disks`
(integration) boots the Linux test guest on a release and a data disk the
library created.

A release candidate is qualified on its image, opt-in, on every engine that
runs Android (the Mac gadgets and JIT engines, the iPad gadgets). A `SIGILL`
anywhere is a blocker. The candidate must show:

- **Boot.** `boot_completed` with the 64-bit and 32-bit ABI lists, the
  secondary zygote running without restarts, SELinux enforcing with no denial
  from the 32-bit processes, and no package compiled on first boot.
- **Applications.** Real 32-bit applications (`armeabi`-only, `armeabi-v7a`-only,
  dual-ABI installed with `--abi armeabi-v7a`, Java-only) select the expected
  primary ABI, descend from the secondary zygote, draw through GLES (and
  Vulkan where used), and leave no tombstone; a deliberate `SIGSEGV` leaves a symbolized
  `crash_dump32` tombstone. The legacy `armeabi` smoke uses three unmodified
  F-Droid archive builds (Simon Tatham's Puzzles 9561.1, Gears ES2 7, Gloomy
  Dungeons 2013.03.07.0416) on the physical iPad, installed with
  `--bypass-low-target-sdk-block` and no `--abi`.
- **CTS.** The 32-bit and 64-bit bionic, binder, OS, JNI, seccomp, ABI-override
  and ASLR modules; a test that fails only on `armeabi-v7a` is triaged before
  acceptance. `CtsUiRenderingTestCases` qualifies on the iPad's gadget engine,
  not on the shared Mac, where `ColorSpaceTests#testMultiAtlasGlyphsWithColorSpace`
  can exceed its 60 s wait when other guests load the host; record such a Mac
  failure as load ([G289](optimization-history.md#ios-graphics-and-later-gadget-work)).
- **Alignment.** The engine probe run from `/data/local/tmp` on both engines
  gives `SIGBUS` for misaligned LDM, STM, LDRD, STRD and VLDM, as on Linux.
- **Cost.** Boot time (cold and warm, ABBA against the previous release on the
  same host build), `MemAvailable`, the zygotes' `dumpsys meminfo`, and image
  sizes.
- **Storage.** The read-only partition hashes match the layout, the guest boots
  on a blank data disk it formats itself, recovery's "wipe data" resets
  `userdata`, a newer release keeps an older instance's apps and data and an
  older release on a newer instance is refused.
- **Hardware video** (a guest started with `--video`). `MediaCodecList` lists
  the four `c2.v4l2.*` codecs with no SELinux or seccomp denial; the encoder
  and decoder classes of the media CTS modules pass on both ABIs through the
  hardware codecs, which the host's video activity log confirms, and the
  software `c2.android.*` results do not regress. Run the decoder performance
  classes on a quiet host.

`android-oobe`, `android-adb-debugging` (integration) each register only when
`UML_ANDROID_OOBE_RELEASE` or `UML_ANDROID_ADB_DEBUGGING_RELEASE` names the
release and the Android macOS executable is built. They boot blank instances
and check the setup wizard's flow and end state, and the product's adb
defaults; `tests/integration/android/run_guest_android_adb_vsock.py` is the
opt-in ADB-over-vsock counterpart of `guest-vsock-stream`.

Every first-party script that reaches a guest over adb uses the one shared adb
server, names the guest with `-s 127.0.0.1:<port>` and at the end only
disconnects its own serial; the stranger-key step of `android-adb-debugging` is
the only private server, and `repository-adb-shared-server` fails any other
script that selects a server or runs `kill-server`. Each Android runner's
default forward port is its guest slot's first port (`GUEST_SLOT_PORT_BASE`)
and a fixed port outside a slot.

## x86 instruction conformance

The x86 gadget engine's external campaign is the pinned FEX `x86-conformance`
suite (x86-64 and i386 modes); for the gadget engine FEX is a test input only,
and FEXCore is linked into nothing but the macOS-only x86-64 JIT. Materialize
each mode with `tools/fex-tests/build-asm-corpus.py` (generated data stays
outside the repository) and run it with `tools/fex-tests/run-asm-corpus.py`;
the JIT takes the same campaign through `uml_x86_64_fex_asm_jit_runner`. The
[campaign README](../tools/fex-tests/README.md) owns the exclusion policy.
Acceptance is every applicable case in both modes passing under that policy,
with no decoder or semantic failures. The macOS unit tree also holds the JIT to
`tests/unit/x86_64/jit_corpus_exclusions.tsv` and
`tier_switch_exclusions.tsv`, whose rows fail the test when they stop matching.
`ctest -N -L x86-64` lists the other x86-64 unit tests, whose headers say what
they check; the `x86-64-jit-*` tests exist only on macOS, and the
`ios-products-carry-no-fexcore` tooling test keeps FEXCore out of every iOS
product.

## Performance and external campaigns

Benchmarks, full LTP, external networking, KDE/VNC/WebGL desktop campaigns, GPU
conformance, SMP stress, official A64 semantics and connected-device runs are
opt-in; run them only when the change or question crosses that boundary.
Geekbench runs the image-local payload, or an attached payload image checked by
SHA-256, through the checked-in workload script with `--no-upload` and
`--iterations 1`. Manual performance tools are under
[`../tests/perf/README.md`](../tests/perf/README.md); record conclusions in
[`optimization-history.md`](optimization-history.md), not raw logs. GPU
capabilities and unsupported Vulkan work are in [`graphics.md`](graphics.md).

## Graphics conformance campaigns

Guest CTS qualification is opt-in, receipt-bound and separate from the
`check-gpu` workflows, which never launch a guest or make a Vulkan support
claim. Every campaign guest is JIT with four vCPUs and 4 GiB. Two rules bound
what any graphics run may claim:

- A version or capability rises only when natural capset discovery and a
  functional test agree. `MESA_GL_VERSION_OVERRIDE`,
  `MESA_GLES_VERSION_OVERRIDE`, forced Virgl capability bits and
  extension-string injection are never evidence.
- A run must exercise the real Virtio-GPU/Virgl path; an app-owned
  framebuffer, a Chromium-specific preload or any other substitute for the
  guest's driver stack invalidates the result for capability purposes.

`tests/integration/gpu/run_vk_cts.py` runs the Vulkan lane,
`run_vk_gl_cts.py` the OpenGL and OpenGL ES lanes, and
`run_graphics_cts_campaign.py` coordinates both; each `--help` describes its
modes, and the campaign runner's `--verify-only` reparses evidence without a
guest. A run is bound to its inputs: the Vulkan lane to
`tests/integration/gpu/vulkan_cts_receipt.json` (sealed image, CTS and Mesa
locks, and the staged KosmicKrisp provider digest, which must match the
verified prefix), the OpenGL matrix to the combined image's receipt and an EGL
configuration inventory from the same emulator, image and provider.
Window-system cases render on real surfaces (native Xorg with Virgl-backed
glamor, the image's KWin nested on Xorg for Wayland); Xwayland and Xvfb are
rejected.

A shard attempt passes only when its guest ran exactly the enumerated
selection once each in released order, delivered its whole QPA, stopped with a
clean Virtio-GPU takedown and reported only statuses the receipt's oracle
accepts. Accepted `NotSupported`, warning and `Waiver` results stay distinct
from `Pass` and prove nothing about an optional feature. A campaign is complete
when every shard has one passing attempt; a complete campaign is not an
all-`Pass`, all-features or Khronos conformance claim. Budget campaign
concurrency from host physical footprint and the `IOAccelerator (graphics)` row
of `vmmap -summary`, not process RSS.

### OpenGL ES3 FBO state-query waiver

`tests/integration/gpu/vk_gl_cts_waivers.json` holds the granted waivers; a new
waiver needs the project owner's grant and an entry there. The one entry covers
the fifteen `dEQP-GL45-ES3.functional.state_query` cases that assert
default-framebuffer state in an `es3-main` run that draws into a
framework-owned framebuffer object, where a conforming driver reports the bound
object. It applies only to a matrix run whose API, configuration name and draw
surface match its scope. The runner installs the waiver file in the guest and
verification accepts `Waiver` only for the listed cases of such a run whose QPA
session header names the granted list; a listed case that still fails, a
`Waiver` anywhere else or a missing header is rejected. `run.json` and the
campaign aggregate count every waived result and set `unwaived_runner_success`
to false. A waived result is not a pass and makes no conformance claim.

### Framebuffer completeness compatibility warning

`KHR-GL46.direct_state_access.framebuffers_check_status` reports
`CompatibilityWarning` when an optional observation sees
`GL_FRAMEBUFFER_COMPLETE`; the runner counts that status as a success and
expects two such observations
([why](graphics.md#guest-capability-boundary)).

## Claiming completion

A passing structural audit does not prove generated-code shape, performance,
kernel boot or guest behavior, and a unit test does not prove an external
campaign. Report only checks actually run and name any input or campaign that
was unavailable. Do not normalize compiler metadata or require byte-identical
independent builds. For a measured A/B, record the revision and dirty state,
configuration, command, result and exit status.
