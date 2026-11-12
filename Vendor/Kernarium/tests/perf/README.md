# Performance tools

This directory owns manual benchmark runners, focused assembly probes and
host-side microbenchmarks. Performance campaigns are opt-in and are not unit
or integration correctness gates. Each runner's `--help` and the docstring of
its script own its options and its output.

## Benchmark policy

- Use matched builds and record the revision and dirty state, relevant
  configuration, host and power state, engine, guest topology, command,
  repeated result, and exit and shutdown status. Diagnostic profiler runs are
  not score evidence.
- Every runner's report line or JSON record carries the vCPU count. Do not
  compare results taken at different counts, and keep one count for a whole
  series. A runner's default count is not a host requirement.
- Run one guest per measurement, alternate the arms of a comparison (ABBA), and
  compare arms round by round.
- Geekbench uses the payload installed in the image (or attached with a
  digest-checked `--payload`) and always runs with `--no-upload --iterations 1`
  ([`guest/geekbench.sh`](guest/geekbench.sh)). A Geekbench payload and every
  image that carries one stay local; `make_geekbench_images.py` refuses to
  write one into the repository outside `.build`.
- Device runs never uninstall the application, never restart the automation
  host, never write into the application container, and never pass
  `--remove-existing-content`. Hold exclusive use of the device for the whole
  run and settle about 45 s between runs, because iOS releases the previous
  guest's wired window asynchronously. Attached Instruments or other sampling
  turns a run into a diagnostic capture.
- Summarize an accepted or rejected trial in
  [`../../docs/optimization-history.md`](../../docs/optimization-history.md).
  Do not add raw profiles, temporary paths, or a separate investigation
  document to the repository.

## Runners

| Script | Purpose |
| --- | --- |
| `run_guest_geekbench.py`, `make_geekbench_images.py` | Geekbench in a disposable overlay-root guest (arm64, x86-64, armhf) and the local images an armhf run needs |
| `run_ios_device_workload.py` | One guest script (Geekbench, the loader corpus, any POSIX script) in Kernarium on an iOS device, delivered as a boot payload that only the automation runtime boots ([Kernarium](../../Kernarium/README.md#automation-runs)) |
| `run_ios_device_android.py` | An adb workload in Andarium on an iOS device ([Andarium](../../Andarium/README.md#automation)) |
| `run_ios_device_discard_bench.py` | UBD discard control on a device |
| `run_steam_boot_bench.py` | Steam appliance boot time, counterbalanced across images |
| `android_launch_suite.py`, `android_launch_apps.tsv`, `android_scenarios/` | Cold application launches in one Android guest boot, from a copy-on-write clone of a prepared source instance |
| `run_guest_command_bench.py`, `run_guest_exec_bench.py`, `run_guest_link_bench.py` | A matched command, cold exec throughput, and cached gadget-block link throughput in a standalone guest |
| `guest_loader_corpus.sh` | Real rootfs programs whose work resembles an application's load phase; compare arms round by round |
| `symbolize_guest_samples.py`, `analyze_gadget_cache_census.py` | Post-processing for the profilers below |
| `uml-host-issue-ratio` | Host 128-bit load and vector ALU issue rates per cycle; run once on the performance cores and once under `taskpolicy -c background` for the efficiency cores |

```sh
python3 tests/perf/run_guest_geekbench.py --help
python3 tests/perf/run_ios_device_android.py --device DEVICE-UDID \
  --app Andarium.app --launch-app settings
```

An Android guest on a device is provisioned after boot when `--erase` starts
a fresh data disk, so the Setup Wizard is not part of the measurement;
`--keep-setup-wizard` measures it. The Steam bench keeps the user-data disk of
a run that does not reach Big Picture (the journal and Steam logs live there)
and reports the first boot's total together with the download window and the
total without it. Its display is VNC with no client attached.

## Native RGB32 texel buffers

The manual `uml-kosmickrisp-rgb32-texel-bench` target links only the verified
KosmicKrisp provider. Configure a `macos-unit-gpu` build with explicit
accepted non-GPU and experimental GPU prefixes, then build and run it
directly:

```sh
cmake --build BUILD --target uml-kosmickrisp-rgb32-texel-bench
BUILD/tests/perf/uml-kosmickrisp-rgb32-texel-bench
```

It compares RGB32 uniform texel views with matching RGBA32 views through one
compute shader and checks every result component exactly. Its two timing
domains (`vulkan-queries` and `metal-whole-submit`) are labelled and must not
be compared, and its repeated-resource figures are not physical memory
bandwidth or guest performance. For the texture-view residency regression, run
`--correctness-only` with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, paired
with `MTL_SHADER_VALIDATION=0`; both runs must pass every exact data and guard
control. Guest coverage is the OpenGL CTS case
`KHR-GL46.texture_buffer.texture_buffer_texture_buffer_range` through
Zink/Venus, or an AArch64 Linux build of the bench run correctness-only
through `tests/integration/gpu/run_vulkan_probe.py`; neither replaces the
native controls. The shader source and generated header are owned here;
regenerate with an explicit offline compiler (`--verify` is a read-only
freshness check):

```sh
python3 -B tests/perf/generate_rgb32_texel.py --glslc /path/to/glslc
```

## Native whole-submit timing

`MESA_KK_DEBUG=submit_timing` enables optional raw Metal commit-feedback
diagnostics on stderr for the whole committed command-buffer batch. It is not
a Vulkan timestamp-query result and is absent from the disabled path. The
KosmicKrisp line and geometry unit executables accept the opt-in arguments
`performance` (render-only Vulkan queries) and `performance-whole-submit`, and
the RGB32 bench uses the same collector through `--timing metal-whole-submit`;
registered CTests never run these modes. The collector requires exactly one
finite, ordered, wall-bounded callback per draw and fails closed otherwise.
Whole-submit intervals include every clear, barrier and copy and are not
comparable with render-only query intervals; no speed ratio is a correctness
threshold.

## Gadget profilers

- **Execution histogram.** A `UML_GADGET_EXECUTION_HISTOGRAM=ON` build counts
  every executed gadget entry by (predecessor, entry) pair and every executor
  round trip by reason into the file named by
  `DARWIN_UML_GADGET_EXECUTION_HISTOGRAM`. The switch changes the executor's
  layout for every target that includes the gadget headers: the unit tests
  that run the executor pass in such a build, the structure tests fail because
  they pin the default stream. Both are diagnostics, not scores.
- **Host sampling of the x86-64 gadget engine.** The bank publishes one symbol
  per gadget, so a sampled PC attributes to the gadget that owned it; the cold
  section names only its start. Threaded dispatch leaves no frame chain (`x29`
  holds the retired-instruction count), but the deepest frame is the real PC.
- **Guest code sampling.** Every arm64 gadgets build carries a statistical
  profiler of translated guest code that needs nothing from the guest. Set
  `DARWIN_UML_GUEST_SAMPLES` to `PATH` or `PATH:HZ`; only the gadgets engine is
  sampled, so a macOS run needs `--execution-engine=gadgets`.
  `symbolize_guest_samples.py` places each block in a library and function
  from the guest's ELF files.

## Cache census

A separate `UML_GADGET_CACHE_CENSUS=ON` build reports what every executor's
cache holds and how it came to hold it. Set `DARWIN_UML_GADGET_CACHE_CENSUS`
to a path (`_SECONDS`, `_WORDS` and `_TOTALS` select the cadence and detail,
and `_TOTALS=1` is what a browser guest on a loaded host can afford) and
summarize the capture with `analyze_gadget_cache_census.py`. A census build
pays on every store and pauses a vCPU for each snapshot, so its timings are
not measurements. `check-unit-census` checks such a build
([testing](../../docs/testing.md)).

## Focused assembly probes

[`probes.tsv`](probes.tsv) is the complete inventory of freestanding A64 probe
sources, with each one's mechanism, required guest feature, runner and
retained purpose. [`CMakeLists.txt`](CMakeLists.txt) builds every row, so a
row without its source fails the build; keep one row per `guest_*.S` file.
Build all probes with the `performance-probe-binaries` target and run one
through the runner `probes.tsv` names. Probes are self-checking and succeed
only when the measured guest command finishes, the guest shuts down, and the
emulator exits zero. They are not CTest registrations.

The `guest_tagged_*_loop.S` probes reach memory through a pointer tagged in its
top byte, as a process with heap pointer tagging does; the emulator ignores
the top byte by default and the runner's `--guest-no-top-byte-ignore` turns
that off. `--probe-argument x` runs the same instructions through an untagged
pointer, their matched control.

## Gadget ceilings

`gadget-ceiling-binaries` builds a matched native/ideal benchmark
(`uml-gadget-ceiling-bench`) with the workloads `add-immediate`, `scalar-mix`
and `simd-mix`. Each body is a `gadget_ceiling_*.inc` macro that the matching
`guest_*_loop.S` probe includes too, so the native control, the ideal stream
and the real guest execute the same words. The ideal mode translates a body
once with the production gadget Builder and bypasses the executor,
translation cache, guest kernel, scheduler and fairness exit; native and ideal
runs must reproduce the same checksum and path counts, and the mix probes
print the same fields from the real guest. Every ratio is a workload ceiling,
not an application estimate. The manual `uml_scalar_ceiling_bench` target
(`scalar_ceiling/`) isolates operand forwarding by swapping forwarding-off and
forwarding-on streams at the same addresses.

```sh
cmake --build .build/presets/macos-unit \
  --target gadget-ceiling-binaries performance-probe-binaries
.build/presets/macos-unit/bin/uml-gadget-ceiling-bench \
  --workload scalar-mix --repetitions 7
```

Run the matching probe unchanged in a real guest with `run_guest_link_bench.py`
(`--binary .../guest_scalar_mix_loop --engine gadgets`, and `--engine jit` for
the matched control); its outer wall time is a conservative end-to-end rate.
