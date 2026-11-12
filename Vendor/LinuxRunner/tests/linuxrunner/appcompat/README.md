# Application compatibility harness

Automated tests of real ARM64 Linux applications from the signed Debian archive, run on the Mac fixture. Results
are Mac fixture evidence only, never physical-iPhone results.

## Pieces
- `apps.py`: test definitions. CLI tests are shell scripts with an expected output marker. GUI tests have a launch
  command, optional setup and functional actions.
- `appcompat_driver.c`: guest X11/XTest driver. It launches an app in its own process group, measures the time to
  the first viewable top-level window, runs actions (click, type, key combos, expect title/window/file, xwd
  capture, shell), closes the app with WM_DELETE_WINDOW, then reports clean or forced shutdown and leftover
  processes as one `APPCOMPAT {json}` line.
- `run_appcompat.py`: host runner. CLI tests run in a fresh runtime each, as the normal user `lrabi_gui`. GUI tests
  run sequentially in one X session (`gui/run_guest_session.py` with the passive RFB probe). It collects the
  runtime compatibility census (`/proc/ish/compat`: missing and stub syscalls, ENOTTY ioctls, faults by process
  name) and host descriptor trends, and writes `results.json`.
- `fixtures/`: small probe programs (Tk input, SDL2 render and input) and a sample PDF.

## Running
```
python3 tests/linuxrunner/appcompat/run_appcompat.py --runtime ISH --rootfs ROOTFS --exchange DIR \
    --output OUT [--suite cli|gui|all] [--only a,b] [--repeat N] --probe PASSIVE_PROBE
```
- Use a **case-sensitive** rootfs volume, as iOS is. Debian packages such as thunar ship names that differ
  only in case (`thunar`, `Thunar`), which fail to install on the Mac's default case-insensitive APFS.
- The inherited host descriptor soft limit defaults to the phone's 2,560 (`--host-nofile`).
- Build the passive probe with
  `clang -fobjc-arc -O2 -I. -framework Foundation -framework Network -framework Security tests/linuxrunner/gui/passive_session_probe.m app/LinuxRunner/RFBClient.m app/LinuxRunner/RFBProtocol.c -o passive-probe`.

## Status words
- PASS: window and every functional check passed, clean close, no leftovers.
- PARTIAL: the window appeared, but a functional check failed or the app needed SIGTERM to close.
- FAIL: no window, or the app or its test never completed.
