# x86-family desktop qualification rootfs

This producer creates the Debian 13 userspace used to qualify the 4 KiB x86
execution boundary with Plasma Wayland, Wine, Steam, and Chromium. The primary
architecture is amd64. Debian's Steam installer requires an i386 runtime, so
the image contains amd64 and i386 ELF payloads and rejects every other ELF
machine type. It never contains arm64 userspace.

Run the producer on a native x86-64 Debian builder as an ordinary user with
passwordless sudo, `debootstrap`, and `erofs-utils`:

```sh
tools/rootfs/x86_64-desktop/build.sh \
  --work /path/to/new-work-directory \
  --output /path/to/new-rootfs.erofs
```

The output is immutable. Its adjacent package and ELF inventories describe the
installed inputs, and the adjacent checksum covers the EROFS image. The GPU
guest harness mounts it as a read-only lower filesystem with a disposable tmpfs
overlay. The image carries no machine ID, so every guest generates its own at
first boot. SDDM logs the `uml` account (passwordless sudo, no password) into
the Plasma Wayland session. The image does not unconditionally enable an `hvc0`
getty: the KDE launcher requests the stock getty only when
`--virtio-console-socket` attaches that device, while `--serial-socket`
exposes the existing `ttyS0` console. A long-running qualification guest is
started with `tools/boot_kde_vnc.py` (see its `--help`) with both sockets.

Debian's Chromium reaches the Virtio video device through VA-API: the producer
builds [`tools/video/va-v4l2`](../../video/va-v4l2/README.md) in the image's
own root against its libva and libdrm, runs the driver's unit test and probe
self-test there, installs `uml_drv_video.so` beside Mesa's drivers and the
`/etc/chromium.d/kernarium-video` file that selects it, and removes the build
tools again.

The first `steam` launch presents a Zenity confirmation; select **Install** in
the visible desktop session. It then downloads Valve's per-user client into the
disposable guest overlay, so this step requires guest networking.
