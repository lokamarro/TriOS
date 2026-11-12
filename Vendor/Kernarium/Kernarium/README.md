# Kernarium

Kernarium is the iOS application that runs one Linux or Android guest on the
Darwin runtime. It targets the iOS device, whose slice also runs on an Apple
silicon Mac in Designed for iPad mode, and the iOS Simulator, where the guest
renders on the CPU for UI tests
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)). Build,
launch, and test commands are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium).
[Andarium](../Andarium/README.md) is the Android-only application.

## Ownership

```text
Kernarium/            SwiftUI application: settings, guest selection, console,
                      microphone setting, background running and picture in
                      picture
Runtime/              runtime framework target, one per kernel flavour
KernariumTests/       hosted application and runtime tests
KernariumUITests/     UI tests
KernariumAutomation/  long-lived UI-automation host for a connected device
AudioInterrupter/     test-support application that takes the audio session
scripts/              launch, packaging, bundle assembly, and automation tools
```

The runtime loader, the guest display and its input, the zstd utility
(`ZstdUtil`), the microphone permission and the shader-cache bound are not in
this directory: [`GuestHost/`](../GuestHost/README.md) holds them, compiled
into this application and into Andarium.

## Runtimes and runs

Kernarium uses `6.18`-series runtime frameworks for Linux
(`KRuntime_6_18_4K`) and Android (`KRuntime_Android_6_18_4K`), both with 4 KiB
guest pages. Their names stay stable across patch releases and kernel commits,
and each carries its kernel ELF in signed Mach-O `__TEXT` (see the
[architecture](../docs/architecture.md#memory-and-executable-code)). The
application embeds every framework, links none at launch, and loads exactly
one for the process lifetime. Packaging includes each runtime's dSYM in its
XCFramework and then strips the framework binary with `strip -S`, which drops
the linker's debug map (it names the build host's directories) and keeps the
UUID the dSYM matches. The dSYM alone also names the kernel's functions, at
their place in `__TEXT,__uml_elf` (`scripts/kernel-dsym-symbols.py`).

Kernarium makes one run per launch. A guest that restarts itself is booted
again inside that run, in the same process, and the run returns only when the
guest is over; the application then says how it ended and asks to be
relaunched, and never starts a second run in the same process. The guest runs
on the named `uml-launcher` thread, which leaves the main thread to UIKit and
MetalKit. The restart loop and its guarantees are in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle); the
application learns of a restart from `set_restart_callback` and re-applies what
belongs to a kernel (the page-cache trimming and the vCPU throttle) to the new
one. The defaults for a new launch are in
`Kernarium/Kernarium/EmulatorConfiguration.swift`.

## Memory

The host GPU's budget is fixed at launch from the guest's memory, the process's
memory limit and three launch parameters, and the settings offer no more guest
RAM than the limit leaves for the chosen vCPU count
([architecture](../docs/architecture.md#memory-and-executable-code) owns the
formulas; [graphics](../docs/graphics.md#device-memory-budget) what the budget
bounds). The application states the parameters in the boot request, asks the
runtime for the plan (`plan_gpu_budget`) before the run and shows a dismissible
notice when the limit leaves the GPU less than its floor; nothing refuses a
launch for lack of memory. The settings ask the loader
(`kernarium_runtime_loader_maximum_guest_memory_mib`) for the RAM bound and a
new configuration's guest RAM
(`kernarium_runtime_loader_default_guest_memory_mib` at the loaded vCPU count,
which also keeps the GPU 1 GiB on a device that offers 4 GiB), because the
runtime framework is loaded once per process. The parameters default to the
conservative launch set the default is measured with; the RAM bound counts none
of them. The `KERNARIUM_GPU_BUDGET_*_MIB` and `KERNARIUM_GUEST_MEMORY_MIB`
environment variables state values for one measurement launch and are never
stored.

## Guest storage

A Linux guest boots an image file in the application's Documents directory:
the one the settings name, or, while they name none, the accepted KDE desktop
image `kde-20260929-r2.erofs`
([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)). A name the
settings already store is kept, and every image in Documents stays selectable.
An Android guest is a `.kern` bundle directory in Documents; the
[Android README](../android/lineage-24/README.md#apple-consumers-and-guest-bundles)
owns its layout. A bundle holds the release's read-only system disk and the
guest's persistent data disk (`guest/data.img`), which the runtime creates at
the first launch and every later launch reopens, so rebuilding the application
or a runtime framework must not erase it and a bundle is never recreated merely
to relaunch it. When copying a bundle to a device, target the complete
`Documents/<name>.kern` destination; never copy into or replace the Documents
directory itself, which can flatten the bundle or destroy other guests.

Launch does not compare kernel, runtime, or rootfs release identities; missing
or malformed inputs fail at their ordinary file or image boundary. The
application chooses the Android data disk's capacity and passes it with the
boot request; the runtime creates the disk or grows it to that capacity, never
shrinks it, and attaches exactly the system disk and the data disk (the
[static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)
owns the layout). Compressed delivery is an application concern: `ZstdUtil`
publishes its destination only after a complete decode.

## Guest boot

A Linux run boots the base archive the runtime embeds
([`tools/rootfs/boot-ramdisk`](../tools/rootfs/boot-ramdisk/README.md))
followed by a segment holding the generic per-run `/init` and its
`/boot.env`; the runtime writes that initrd to its temporary directory and
removes it when the run returns. The parameters mount the image read-only on
`ubd0` beneath an overlay whose writable layer is an ext4 filesystem formatted
afresh on `ubd1` (the sparse `kernarium-overlay.ext4` the application recreates
in its temporary directory at each launch). `/init` then switches root into the
image's own `/sbin/init`, which boots `graphical.target`.

Nothing is typed on the console and the runtime edits no image: accounts,
device groups, getty and display-manager policy, network and resolver belong
to the image. The KDE, Phrog and postmarketOS images Kernarium boots carry
that setup ([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)); the
unified `rootfs.erofs` carries none of it and boots to its own defaults.

## Automation runs

The release runtime accepts no host payload and carries no test campaign.
Test and benchmark drivers package one of two automation variants instead, and
Andarium's App Store configuration a third runtime; the packaging script
(`scripts/package-ios-frameworks.sh`) checks that a release runtime carries
none of the settings below.

- **Linux automation**: the `kernarium-ios-device-automation` preset
  (`UML_KERNARIUM_AUTOMATION_HARNESS`), packaged with `--automation`. Only it
  reads `KERNARIUM_BOOT_PAYLOAD` (a plain file name in Documents: a padded newc
  segment of at most 64 MiB, written by `uml-initramfs`, appended after the
  runtime's own so its files replace the runtime's), `KERNARIUM_SYSTEMD_UNIT`
  (the target booted instead of `graphical.target`) and
  `KERNARIUM_PAYLOAD_REMOVE=1` (the runtime deletes the payload once it holds
  it, because `devicectl` cannot remove a file in an application container). A
  payload also attaches the artifact relay, whose frames the runtime prints on
  standard output. `tests/integration/support/kernarium.py` writes payloads for
  the lanes in [`testing.md`](../docs/testing.md#apple-builds-and-kernarium)
  and [`tests/perf/README.md`](../tests/perf/README.md).
- **Android automation**: the `kernarium-ios-device-android-automation`
  preset, packaged with `--android-automation`. The release runtime always
  starts the guest with `androidboot.kernarium.adb_auth=1`, so a fresh bundle's
  first ADB connection waits for the "Allow USB debugging" prompt. Only the
  automation runtime reads `KERNARIUM_ANDROID_ADB_AUTH`: `0` starts one launch
  with authorization off, as the macOS launcher does. It refuses that unless
  `KERNARIUM_HOST_FORWARD` is unset or listens on loopback, because
  authorization off gives whatever reaches the forward an unauthenticated shell
  (and `adb root` on a userdebug image).
- Both automation runtimes also read `KERNARIUM_BACKGROUND_JOB_AT_START` and
  `KERNARIUM_BACKGROUND_HOLD_DELAY_MS`, which start a background job with the
  guest and move the GPU hold for a measurement; the packaging script refuses a
  release runtime that contains their names.
- **App Store runtime**: the `kernarium-ios-device-android-app-store` preset
  (`UML_APP_STORE_RUNTIME`), packaged with `--app-store --runtimes android` for
  Andarium's App Store configuration; it conflicts with the automation harness.
  Every environment variable that tunes, observes or drives a run is read as
  unset (the `KERNARIUM_` knobs and the `UML_` and `DARWIN_UML_` diagnostics;
  `TMPDIR` stays). It also carries none of the debugging support, so a shipped
  application cannot capture its screen, inject input, profile itself or log
  its memory: the frame mirror, console input (the console output relay stays),
  the guest sampler, the footprint log, the virtual Jetsam, the activity logs
  and the frame statistics reports. Configuring it with
  `UML_GADGET_CACHE_CENSUS` or `UML_GADGET_EXECUTION_HISTOGRAM` fails. The
  runtime API table keeps its layout and version, so `set_frame_sink` and
  `send_console_input` answer `-ENOTSUP`; touch and key input, adb over vsock,
  the statistics and motion channels and the lifecycle, trim and budget calls
  are product features and stay. `Andarium/scripts/check-app-store-bundle.py
  --runtime`, which packaging runs, refuses the symbols and log text of what is
  left out. Switches that third-party libraries read from the environment stay
  in the shipped libraries; nothing reaches them on an installed application.

The launcher alone tells an Android guest how adbd is reached, on its kernel
command line; a release's command-line file cannot set these arguments. The
boot request carries the adb host key to authorize for that boot
(`android_adb_public_key`, runtime API 19) and the host's time zone
(`android_time_zone`, API 23: Kernarium sends none,
[Andarium](../Andarium/README.md#runtime) the system's zone), and a launch with
no `KERNARIUM_HOST_FORWARD` has adbd listen on vsock alone. The runtime
validates the values (`KernariumAdbChannel.hpp`, `KernariumGuestTimeZone.hpp`).

## Devices and display

Kernarium attaches:

- an absolute touchscreen and an evdev keyboard (Linux key codes 1 through
  127, plus Back and Home for Android's navigation keys);
- Virtio-Sound playback and capture, which records silence unless the user
  turns on Microphone;
- an Android-only Virtio battery reflecting the Apple host's battery;
- SLiRP networking from the verified iOS prefix;
- a Virtio vsock device ([the device](../darwin/device/vsock/README.md)), the
  last Virtio device, through which the application opens streams to guest
  ports (`connect_vsock`, runtime API 19; the guest cannot connect to the
  application);
- a Virtio-GPU Metal framebuffer; and
- the guest's `ttyS0` stream mirrored to a terminal view.

Point display mode maps safe-area points to guest pixels at 160 DPI; Retina
mode multiplies both geometry and density by `nativeScale`. The GPU's EDID and
Android's logical density on the kernel command line use that launch geometry.
The touchscreen's absolute range is fixed at 0 through 32767 on both axes, and
Kernarium maps the guest's visible rectangle in the view onto it.

A host short of the space a hibernation image takes quiesces with
`quiesce_resident` (runtime API 25), which keeps the guest's RAM in the process
([`docs/architecture.md`](../docs/architecture.md#hibernation)).

A contact the host takes away, rather than ends (UIKit's cancelled touches, a
display mode change, the view leaving its window), is cancelled with
`cancel_touch` (runtime API 24), not released: the touchscreen turns the slot
into a palm before the lift, so Android reports `ACTION_CANCEL` and libinput a
cancelled touch, and no tap or click follows (`guest-touch-cancel`,
[`docs/testing.md`](../docs/testing.md)). Known limits: X11 has no per-touch
cancel, so an X11 application under Xwayland can still read a cancelled touch
as a click, and Android's Recovery menu ignores the tool type, so a cancelled
still tap there still selects the item under it
([`android/lineage-24/README.md`](../android/lineage-24/README.md)).

The Metal and Mesa shader cache policies are in
[`graphics.md`](../docs/graphics.md#metal-shader-cache). The guest view follows
the scene's geometry (windows, tiling, Slide Over, Full Screen Apps and
rotation). Kernarium runs one guest per process and so has one window: its
scene manifest does not support multiple scenes.

### Microphone

The Microphone switch in Emulator settings is off by default; while it is off,
or iOS has not granted access, a guest that records gets silence and nothing
opens the microphone. Turning it on is the one place iOS is asked for access
(`MicrophoneSetting`); the application then tells the runtime
(`set_microphone`), which never asks. A guest kept running in the background
may keep recording under the `audio` background mode.
[`../darwin/device/audio/README.md`](../darwin/device/audio/README.md#microphone)
owns the device side.

## Lifecycle

Kernarium asks the guest to write back when the application loses focus,
quiesces it before the application is backgrounded or terminated, and resumes
it on foregrounding. A key press also wakes a guest that suspended itself, as a
phone wakes to its keyboard; a touch does not. The runtime side of sync,
quiesce, resume, and wake is in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle).

iOS revokes the host GPU within seconds of `didEnterBackground`, and a guest
request that reached Metal afterwards would make the guest's Vulkan device
lost for good. The application therefore holds the guest's GPU requests
(`GuestGPUHold`) inside `didEnterBackground`, before it quiesces, and releases
them when it returns to the foreground. The hold and the quiesce share the
handler's four-second budget; the quiesce carries on under the `guest-sync`
background task. Guest page-cache trimming stays off in the foreground, and
quiescing trims once by itself.

On a device the quiesce ends by hibernating the guest's RAM, so that a paused
Kernarium no longer carries the guest's wired memory
([`../docs/architecture.md`](../docs/architecture.md#hibernation));
`KERNARIUM_GUEST_HIBERNATION=off|on` overrides it. iOS may still end a
suspended application, and the guest goes with it: a marker file records the
background edge, and the next launch says the guest restarted because
Kernarium was closed in the background.

### Keeping a guest running in the background

A guest pauses in the background unless its owner chose otherwise. Settings
holds one mode per guest, stored under the guest's root filesystem name, and
`off` is the default because running costs battery. `BackgroundController`
decides what the lifecycle edges do and acts only through
`BackgroundEffects`, which the unit tests replace.

- `pictureInPicture` keeps the guest running while iOS shows its display in a
  Picture in Picture window. The runtime mirrors the display to a sink the
  application sets (`kernarium_runtime_loader_set_frame_sink`,
  [`graphics.md`](../docs/graphics.md)), which feeds an
  `AVSampleBufferDisplayLayer`. It needs the `audio` background mode in
  `Info.plist` and a playback audio session that mixes with other audio.
- `keepRunning` keeps a guest working without a window for a time its owner
  chose, through one `BGContinuedProcessingTask` (`BackgroundJob`) that the
  user starts with a button tap. iOS grants that task no GPU, so the guest's GPU
  requests stay held for the whole stay.
- A guest kept running has its vCPU threads at background priority
  (`set_background_throttle`) and trims its page cache periodically
  (`set_page_cache_trimming`). An Android guest in `keepRunning` has its screen
  turned off with its power key before the GPU is held, so it can suspend
  itself; work that should make progress there has to hold a wakelock.
  `BackgroundPolicy.swift` and `GuestGPUHold.swift` document the order of the
  edges.

### Low space on the device

A guest's disk grows as the guest writes (the Linux overlay in the temporary
directory, an Android bundle's `data.img`), and a write the device cannot take
stops the guest. The thresholds and the pause itself are
[GuestHostCore](../GuestHost/README.md), which Andarium shares.

- A start needs room for the guest's first writes (`StartSpace`: 1.5 GiB, and
  3 GiB for a bundle that has no data disk yet). On a device short of it a
  notice covers the guest views, nothing starts, and Check again tries once
  more.
- While a run is on, `GuestSpacePause` reads the free space now and then and
  pauses the guest below 1 GiB. The runtime suspends it with its RAM kept in
  the process (`quiesce_resident`), because a hibernation image is the write
  the device has no room for; the guest's processor time standing still
  confirms the suspension, and a screen over the guest offers Resume and Shut
  down. A guest that does not hold still is called so, with Shut down and
  Check again.
- Resume reads the space again and wakes the guest at 1.5 GiB or more. Shut
  down is the run's stop, the same as leaving the guest: the suspension
  already wrote the guest's storage back, nothing is asked of the guest, and
  the run ends with the notice that the guest shut down. Kernarium has no
  clean power-off request.
- No lifecycle edge wakes a paused guest. While `BackgroundController
  .spaceHeld`, an edge holds the guest's GPU and does nothing else, in every
  background mode, so the guest stays suspended and resident in the
  background; the Picture in Picture window and the keep-running job end when
  the pause begins. The watch reads the space only while the policy leaves
  the guest running.
- `KERNARIUM_SPACE_HARNESS=1` makes a file `free-space-mib` in Documents,
  holding a number, stand for the device's free space while it exists, and a
  file `space-answer` there (`resume`, `shutdown` or `check`) answer the
  screen once it offers that answer, so a test drives the pause without
  filling the disk.

## Statistics

An application that shows how hard the guest works asks the runtime, which
measures nothing between calls (runtime API 20, `read_statistics` and
`read_guest_memory`). Two readings of one boot give the share of the guest's
processors that computed and the rate of presented frames.
`read_guest_memory` asks the guest kernel through the management console, so it
is never sent where it could wake a guest that sleeps (`-EAGAIN`) and is called
away from the main thread. `KernariumStatistics.hpp` holds the rules, with
`kernarium-statistics` as their test. The reading also counts the graphics
allocations the host has refused the guest (`gpu_memory_refusals`, runtime API
26), which Andarium turns into a notice, and the host GPU's memory budget with
the usage it refuses by (`gpu_memory_budget_bytes` and `gpu_memory_used_bytes`,
runtime API 27), which Andarium shows as VRAM; Kernarium reads none of them.
`read_graphics_memory` (runtime API 28) adds the budget's holders by guest
process, and whether the guest's own low-memory killer reclaims them
(`guest_reclaims`, API 29;
[`docs/graphics.md`](../docs/graphics.md#device-memory-budget)).

## Simulator

The Simulator build is for UI tests of the application and its Android guest.
Configure and build a `kernarium-ios-simulator*` runtime preset, package it with
`scripts/package-ios-frameworks.sh --sdk simulator`, and build the scheme for a
concrete Simulator, or run the tests with `scripts/run-ios-simulator-tests.sh`
([`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium)). The
Simulator renders on the CPU
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)).

## Scripts

Each script documents its options in `--help` or its header. Two facts the
headers do not make obvious: `package-ios-frameworks.sh` writes one shared
output directory, so after `--runtimes android` for Andarium package the full
set again before building Kernarium; and `audio-interrupter.py build` signs
with the team of
[`GuestHost/Config/Signing.local.xcconfig`](../GuestHost/README.md#signing) or
`UML_DEVELOPMENT_TEAM`.
