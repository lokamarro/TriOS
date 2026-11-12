# Android WebGL memory workload

`AndariumWebGLProbe` runs the public WebGL Aquarium with 5,000 fish in Android
WebView at the guest display's pixel dimensions. It requires hardware
rendering and an intact WebGL context; this is a stress workload, not an
assertion of interactive performance.
`Andarium/AndariumTests/RetinaMemoryTests.swift` drives it on a physical iPad.

Build this directory as an explicit Soong module in the configured Android
workspace (`m AndariumWebGLProbe`), with a temporary source copy at
`test/andarium-webgl`. It is not a product package. Remove the source copy and
any installed product-output copy after exporting the APK; the target-files
verifier rejects a release that still holds it. Copy the APK to Andarium's
`Library/Caches/andarium-webgl-probe.apk` before each run; the test consumes
it. The test needs network access to load the Aquarium.

Run only `AndariumTests/RetinaMemoryTests` with `xcodebuild test`, passing
these environment variables with the `TEST_RUNNER_` prefix (the test's source
lists the optional profiles):

```sh
ANDARIUM_RETINA_MEMORY_TESTS=1
ANDARIUM_GUEST_INSTANCE=retina-memory
ANDARIUM_RETINA_PROFILE=1
ANDARIUM_WEBGL_PROFILE=1
KERNARIUM_GUEST_MEMORY_MIB=<guest memory in MiB>
KERNARIUM_VCPU_COUNT=<guest vCPUs>
KERNARIUM_FOOTPRINT_LOG_MS=1000
```

The test requires the guest to have the memory and vCPU count the launch
states and records both in every snapshot. Collect `tmp/RetinaMemory/` from
the app container afterward. Compare settled allocations across rounds and
after shutdown; a process peak alone cannot establish a leak. Shutdown
requires zero wired pages and no dirty or swapped guest-domain or scratch-heap
pages.

To clean up the isolated guest, launch Andarium with
`ANDARIUM_GUEST_INSTANCE=retina-memory` and `ANDARIUM_ERASE=1` in its launch
environment, without a start mode. Its erase action removes the test disk
without booting Android. Relaunch without those overrides to return to the
primary guest.

Do not use CoreDevice's `copy to --remove-existing-content` for this cleanup.
With the `appDataContainer` domain it can clear the entire container, including
the primary guest, even when a subdirectory destination is supplied.
