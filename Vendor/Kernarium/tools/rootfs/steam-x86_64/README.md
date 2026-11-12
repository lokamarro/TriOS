# Steam x86-64 gaming rootfs

This producer builds a Debian 13 amd64 Steam appliance with Gamescope,
Xwayland, and Valve's own native x86-64 Linux Steam client started with
`-gamepadui`. It installs no desktop environment and no compatibility tool:
the guest runs the client Valve ships for this architecture, and Steam manages
Proton itself. The image has no i386 multiarch and no 32-bit system loader; the
audit rejects any i386 ELF outside Steam's own bundled compatibility paths and
every non-x86 ELF.

The default build retains Debian Mesa, which Gamescope cannot use for a Vulkan
session. The Vulkan build imports a receipt-verified Mesa/Venus and
Vulkan-Loader runtime built for x86-64 through that guest's own importer,
`../vulkan/install-runtime.py`; the gaming session then selects Zink for
Xwayland and OpenGL clients. This qualifies neither game rendering nor Vulkan
conformance.

The image uses the accepted 4 KiB x86-64 kernel and the `uml-darwin` emulator.

## Inputs and build

[`sources.json`](sources.json) locks the Debian suite, the Steam release
channel, the pinned client version and manifest digest, and the Steam runtime
snapshot; [`steam-client.manifest`](steam-client.manifest) is the unmodified
upstream client manifest. Every package it names is downloaded from Valve's own
content service and verified against the SHA-256 in the manifest. The seeded
client is therefore one release behind Valve's current publication, and the
first boot downloads that difference.

Build on native x86-64 Debian 13 as an ordinary user with passwordless sudo.
Preparation is the only network stage. This prototype runs preparation from the
producer directory instead of through a `dependencies/prepare.sh --product`
selector.

```sh
tools/rootfs/steam-x86_64/prepare-inputs.py --root /work/steam-x86-inputs
tools/rootfs/steam-x86_64/build.sh --inputs /work/steam-x86-inputs \
  --work /work/steam-x86-image --output /work/steam-x86_64-gaming.erofs
```

For the Vulkan variant, add `--vulkan-root` and `--vulkan-receipt` pointing at a
sealed x86-64 Vulkan guest tree and its receipt. Build work and output paths
must be absent. `prepare-inputs.py --root PATH --verify` performs read-only
payload verification and is what `build.sh` runs before it touches anything.
The splash renderer and the VA-API backend are compiled offline in the builder
root, so they link exactly the libraries the image ships; the builder never
enters the image.

## Image and startup contract

`overlay/` holds what the producer adds to the Debian root: the image-owned
init and its storage preparation, the Gamescope session and its supervisor, the
`steam` launcher, the splash hand-off, and the V4L2 decoder discovery rule.
`splash/` is the C++20 Kernarium boot and installation renderer. The output
carries SHA-256, package and binary inventories, and input provenance sidecars;
the audit also requires the native x86-64 Steam client, Gamescope, the splash
renderer and the VA-API backend with its session wiring, and rejects an
installed desktop environment.

- **Storage.** The EROFS root stays read-only. A separate raw GPT disk holds one
  ext4 partition mounted at `/userdata`, whose `home`, `var` and `root` are bind
  mounted at `/home`, `/var` and `/root`; `/run` and `/tmp` are tmpfs. The
  Steam client installation, games, settings and saves live on the data
  partition.
- **Data-disk safety.** The image-owned init formats only a new partition
  carrying the launcher's initialization marker, repairs existing ext4 with
  `e2fsck -p`, and stops boot with a visible recovery message rather than
  erasing an unrecognized disk. Later boots never reseed an existing home.
- **Session.** `steam-gaming.service` starts a Gamescope session as the
  image-owned `uml` user. The session is ready when the X window
  `Steam Big Picture Mode` becomes viewable.

## Video decode

Valve's x86-64 CEF has no stateful V4L2 decoder, so its only hardware path is
VA-API, and neither Debian nor upstream has a VA driver for this device. The
image builds and installs its own,
[`tools/video/va-v4l2`](../../video/va-v4l2/README.md#in-the-steam-x86-64-image),
which also records how the session selects it.

## Interactive boot

```sh
python3 tools/rootfs/steam-x86_64/boot.py \
  --emulator /path/to/uml-darwin \
  --rootfs /path/to/steam-x86_64-gaming.erofs \
  --userdata /path/to/your-userdata.img --size-gib 32 \
  -- --video --gpu-display=native
```

The launcher owns the storage, init, console, memory and engine arguments and
rejects them in the trailing pass-through list. It creates a sparse GPT disk
only when `--userdata` is absent and holds an exclusive launch lock; the size
option applies at creation only and never resizes or resets existing data. Keep
the data disk backed up separately from the replaceable rootfs.
