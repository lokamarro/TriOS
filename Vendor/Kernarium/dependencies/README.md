# External dependencies

This directory owns external-library source identities, generated patch
series, preparation, offline build recipes, platform contracts, and installed
prefix verification. Kernel provenance is separate under
[`../kernel/`](../kernel/README.md).

[`manifest.tsv`](manifest.tsv) is the exact source inventory. Each row pins one
upstream Git commit, the products that consume it, and any ordered local patch
series. Production must not discover libraries from Homebrew, ambient
`CMAKE_PREFIX_PATH`, or unrestricted `pkg-config`; consumers use only verified
private prefixes through the CMake targets in this directory.

## Products

Each product is prepared with `prepare.sh --product NAME` and built by the
script below; per-platform contracts are under [`platforms/`](platforms).

| Product | Builder |
| --- | --- |
| `macos` | `build.sh` |
| `apple-gpu-vulkan-experiment` | `build-apple-gpu.sh --platform macos\|ios\|ios-simulator` |
| `ios-network` | `build-ios-network.sh --platform ios\|ios-simulator` |
| `ios-storage` | `build-ios-storage.sh` |
| `ios-compression` | `build-ios-zstd.sh` |
| `kernel-host-tools` | `build-kernel-host-tools.sh`, after `prepare.sh --archive-manifest kernel-host-tools/archives.tsv` |
| `x86-conformance` | none: prepared only |

- `macos`: non-graphics static host libraries at
  `.build/dependencies/macos-arm64-27`. It also carries FEXCore for the
  macOS-only x86-64 JIT (no iOS product names it) and libslirp.
- `apple-gpu-vulkan-experiment`: the macOS and iOS device builds, where Mesa
  supplies the KosmicKrisp Vulkan driver and the Zink Gallium OpenGL driver on
  top of virglrenderer, libepoxy and the Venus protocol; and the iOS Simulator
  build, a software variant with no Vulkan, Venus or KosmicKrisp whose Mesa
  renders on the CPU (see
  [`../docs/graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)).
  A prefix's `gl_provider` contract field names the one host GL provider it
  carries. New prefixes carry matching dSYMs under `dSYMs/`; iOS framework
  packaging requires them, so rebuild an older prefix without symbols before
  archiving an application.
- `ios-network`: the iOS GLib/libslirp network closure.
- `ios-storage`: the patch-managed GPT fdisk core as a static
  `GPTFDisk.xcframework`. The standalone macOS Android launcher runs an
  installed `sgdisk` instead (see the
  [static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)).
- `ios-compression`: the decode-only zstd library as a static
  `LibZstd.xcframework`.
- `kernel-host-tools`: pahole and the static libelf the Darwin kernel build
  uses for BTF, at `.build/dependencies/kernel-host-tools`; patch-managed
  Darwin ports that run only on the build host.
- `x86-conformance`: the test-only FEX and K x86-64 corpora and their pinned
  producer inputs, used to generate the checked-in x86 unit corpus (see
  [`../tools/fex-tests/README.md`](../tools/fex-tests/README.md)).

The libslirp series is [`patches/libslirp`](patches/libslirp/series.tsv); the
user-network contract it provides is in
[`../darwin/README.md`](../darwin/README.md). The FEX series is
[`patches/fex`](patches/fex/series.tsv). It touches nothing the
`x86-conformance` producers read, so one prepared FEX source serves both
products. On Apple, FEXCore maps, frees, and write-enables executable memory
only through hooks its embedder installs, and prefix verification rejects a
FEXCore archive that toggles JIT write protection itself.

Kernarium's serial console uses libghostty-spm's `GhosttyTerminal`, an Xcode
Swift Package pinned exactly in the Kernarium project and fetched by Swift
Package Manager. It is not part of the emulator dependency prefixes.

## Prepare, build, and verify

`prepare.sh` is the only dependency command allowed to access the network. It
fetches pinned commits into the local cache and creates verified prepared
repositories. All build scripts are offline.

Use `--no-fetch` for offline preparation and `--verify` or `--verify-only` for
read-only checks; each script's `--help` owns paths and options. A build
verifies its inputs before it creates anything. Two prerequisites cannot be
guessed:

- the `x86-conformance` product needs a case-sensitive `--root` (its K
  semantics source has paths that differ only by case) and preparation refuses
  that source on a case-insensitive volume;
- a cache entry must hold every object of its pinned commit; preparation
  refuses a partial one, such as a blob-filtered clone, and does not repair it.

A prepared source is identified by the tree its series records in
`series.tsv`. One whose tree no longer matches is replaced only by
`prepare.sh --recreate`, and only when it is clean and its branches point at
commits the cache holds. An installed prefix is never replaced automatically:
build the new one beside it. Prefix verification checks the manifest rows, the
patched sources' trees in `share/uml-dependencies/source-trees.tsv`, platform
and deployment target, expected payloads, framework install names, and the
absence of undeclared or legacy providers. Every macOS and iOS product prefix
builds and verifies at a 27.0 deployment target; the kernel host tools are the
exception. A prefix compares only the manifest rows its product consumes, so a
change to another product's row does not invalidate it, while a change to one
of its own rows requires a rebuild.

Additional build-only archives use
`dependencies/prepare.sh --archive-manifest PATH --cache CACHE` with a
`name`, `url`, `sha256` manifest and install nothing; see
[`kernel-host-tools/archives.tsv`](kernel-host-tools/archives.tsv) and
[`llvm/archives.tsv`](llvm/archives.tsv). `--resolve-ref` and
`--materialize-tag` discover and pin a release tag's immutable identity.

## No build-host paths

A binary that ships must hold no path of the machine or prefix that built it.
The builders configure each library whose code derives directories from its
prefix (GLib, Mesa's OpenGL libraries, virglrenderer, FEXCore) for
`NO_HOST_PREFIX` (see [`build-common.sh`](build-common.sh)) and install it with
`DESTDIR`, so a prefix can be built beside another and renamed into place.
`Andarium/scripts/check-app-store-bundle.py` refuses a home, volume or
temporary path in the host code of the application and its frameworks. The
platform contracts' `common_recipe` is the recipe identity a prefix was made
with.

## Apple GPU prefix

`apple-gpu-vulkan-experiment` is built for macOS and the iOS device from one
source matrix and one set of patches; `--platform ios-simulator` selects the
software variant. The format-4 platform contract sets
`gl_provider=zink-kosmickrisp` and `vk_provider=venus-kosmickrisp`. Prefix and
runtime verification require exactly that provider set and reject a second
Vulkan provider, an ambient loader, a `dlopen` or `dlsym` import in the
renderer or KosmicKrisp, or a duplicate framework copy. Venus codecs are
generated from the authenticated SDK registry at build time and are never
installed payload.

The maintained virglrenderer series and the guest Mesa series under
`tools/rootfs/vulkan/` form one compatibility epoch: changing either
invalidates the prefix and the Vulkan rootfs receipt. The pairing is local to
this project and is not a formal Vulkan conformance claim.

The Venus protocol series is owned by
`tools/rootfs/vulkan/patches/venus-protocol`; the manifest's explicit
`rootfs-vulkan:` namespace references that single owner. The series carries
generator inputs only, never generated headers (`tools/rootfs/vulkan/verify-inputs.py`
rejects them). A protocol topic changes guest and renderer together and needs
a matching guest epoch.

Host and guest build Mesa from one topic branch on the same pinned commit. The
guest series (`tools/rootfs/vulkan/patches/mesa`) is the lower segment and
`patches/mesa` holds only the host topics after it, so a host-only change never
alters the guest tree its CTS receipts name. The Vulkan headers and registry
Mesa bundles are the pinned SDK's own, carried by the guest segment's first
topic; moving the `vulkan-headers` pin refreshes it.

KosmicKrisp's Mesa regressions build with `-Dbuild-tests=true` and
`-Db_ndebug=false` (assertions must stay active) and run with
`meson test -C BUILD --suite kosmickrisp`, `vulkan`, `util` or `gallium`. The
host GL tests are registered in `check-gpu`. KosmicKrisp's runtime behaviour is
specified in
[`../docs/graphics.md`](../docs/graphics.md#kosmickrisp-driver-contracts).

KosmicKrisp's device library is checked-in SPIR-V that the build does not
compile. [`kosmickrisp-spirv/generate.py`](kosmickrisp-spirv/generate.py)
regenerates it on an arm64 Debian host with the packages pinned in
[`kosmickrisp-spirv/toolchain.tsv`](kosmickrisp-spirv/toolchain.tsv) and
rewrites [`kosmickrisp-spirv/record.tsv`](kosmickrisp-spirv/record.tsv). The
`dependency-kosmickrisp-spirv` tooling test fails when a topic changes its
inputs without a regeneration; amend the blobs into the embed topic and
regenerate the series.

## Patch maintenance

Patch files are generated mail artifacts. Modify the clean Git topic checkout,
then regenerate the owning series:

```sh
dependencies/rebase-patches.sh \
  --dependency NAME --source /path/to/topic-checkout
```

The regenerator exports the pinned commit's descendants through
`scripts/patch_series.py`, which rewrites only the patches whose commits
changed and records the resulting tree in `series.tsv`. Never edit patch bytes
or `series.tsv` by hand. Series replay commits as whoever Git resolves for the
caller, and export refuses a placeholder author; see
[`kernel/mainline-6.18/README.md`](../kernel/mainline-6.18/README.md) for the
shared contract.

A manifest series field may join ordered layers with `+`. Each layer applies
to the tree the layers before it produce, and the last layer, which `patches/`
owns, records the tree of the whole stack (`series-field.sh` holds the
grammar). The only lower layer allowed is the paired guest's Mesa series; a
changed guest layer is regenerated first with
`tools/rootfs/vulkan/rebase-patches.sh --head GUEST_END`.

Changing `manifest.tsv` or a build recipe invalidates the corresponding
installed receipt, and so does a series change that produces a different tree.
A series change that keeps the tree, such as a reworded message or a folded
follow-up, leaves every prefix valid. Rebuild an invalidated prefix and run its
tooling checks in the same change.

## Host prerequisites

Dependency preparation and builds require the Apple command-line tools plus
CMake 3.27 or newer, Ninja, GNU Make, Meson, Python 3.11 or newer,
`pkg-config`, Git, rsync, Autoconf, Automake, gettext, GNU libtool, gperf, and
groff. Beyond that:

- Configure needs Python 3.11 or newer; macOS's own `python3` is 3.9, so put a
  newer one first on `PATH` or pass `-DPython3_EXECUTABLE=<python>`.
- Git needs a configured identity with a real email address: the patch-series,
  kernel, Android, and dependency tooling tests commit fixtures as whoever Git
  resolves, and patch export rejects a host-derived placeholder. `check-tooling`
  also needs `autopoint` from gettext.
- Apple GPU builds need Xcode's Metal Toolchain component
  (`xcodebuild -downloadComponent MetalToolchain`).
- Meson, Mako, PyYAML, and packaging may come from an optional virtual
  environment at `.build/gpu-tools-venv`; its `bin/meson` is preferred over
  `PATH` and `UML_DEPENDENCY_MESON`.

Some workflows need further tools and fail naming the missing one:

| Tool | Needed by |
| --- | --- |
| `ld.lld` (LLVM lld) | integration guest payloads, the x86 probe corpus |
| erofs-utils (`mkfs.erofs`, `fsck.erofs`) and e2fsprogs (`debugfs`, `e2fsck`, `resize2fs`) | `tests/perf/make_geekbench_images.py`; erofs-utils alone for `guest-erofs-lz4` |
| gptfdisk (`sgdisk`) | the Android launcher, `tools/boot_android.py` |
| GNU coreutils `timeout` | the oracle scripts under `tools/arm64-tests/` |
| OpenSSL (`openssl`) | `tools/rootfs/provision.py` |
| LLVM 22.1.8 `llvm-mc` | the instruction-table freshness tests, which skip without it |
