# Android storage accounting test

`AndariumStorageTests` checks that Android reports the sizes of the mounted
filesystems exactly: the primary-storage helpers and `StorageStatsManager`
against `StatFs` of `/data`, `/system`, `/system_ext`, `/product`, `/vendor` and
`/odm`. It guards the `frameworks/base` storage topic (route `p0007`). It is a
test of the framework, not a product package, and is never installed in a
release image: the target-files verifier refuses `AndariumStorageTests` in any
partition and under `DATA`.

The APK is signed with the AOSP test key, the default certificate, like the
other probes here, and never with a release key. It calls one hidden API
(`StorageManager.getPrimaryStoragePathAndSize`), which a test-key application
may not do unless the guest's `hidden_api_policy` global setting is 1; the
Andarium test sets it for the run and puts it back. Build it as an explicit Soong
module in the Android workspace (`m AndariumStorageTests`) after copying this
directory to `test/andarium-storage`, then remove that copy. Run it with
[`AdbOverVsockTests/androidReportsExactStorageSizes()`](../../../../docs/testing.md#andarium).
