# Android motion input probes

`AndariumMotionProbe` is a foreground application that reads the accelerometer
and gyroscope through Android's public SensorManager API.
`Andarium/AndariumTests/MotionOverVsockTests.swift` drives it on a physical
iPad. The foreground application is necessary for sustained validation:
Android can disable a plain shell UID's sensor subscription after that UID
becomes idle, independently of host delivery.

Build this directory as an explicit Soong module in the Android workspace
(`m AndariumMotionProbe`), with a temporary source copy at
`test/andarium-motion`. It is not a product package and is never installed in
a release image. After exporting the APK, remove that copy and
`system/app/AndariumMotionProbe` below the product output before a release
build; the target-files verifier rejects a release that still holds it.

Copy the APK into Andarium's `Library/Caches/andarium-motion-probe.apk` with
`devicectl device copy to`; each run consumes it, so copy it again before the
next. From the repository root, run:

```sh
TEST_RUNNER_ANDARIUM_MOTION_TESTS=1 xcodebuild test \
  -project Andarium/Andarium.xcodeproj -scheme Andarium \
  -destination 'platform=iOS,id=<device-id>' \
  -only-testing:AndariumTests/MotionOverVsockTests
```

The separate hands-on check adds `TEST_RUNNER_ANDARIUM_MOTION_INTERACTIVE=1`
and `TEST_RUNNER_ANDARIUM_ROTATION_VISUAL=1`; a person tilts and rotates the
iPad while the probe is visible, and a stationary device does not pass.
