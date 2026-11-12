# Desktop images

This directory owns the derivation of the desktop images Kernarium boots:
KDE Plasma, Phosh greeted by Phrog, and postmarketOS with Sxmo. None of their
sources is built in this repository. The KDE source is the pinned unified
`rootfs.erofs`; the other two are user-supplied images. Each source lacks
some policy a Kernarium guest needs, and the derivation bakes that policy
into a new image, so a launcher never edits an image while it boots.

[`images.json`](images.json) records, for each image, the source file's size
and SHA-256, the setup script, the compressor that reproduces the source's
layout, and the accepted output with the digest of the setup that produced it.
[`derive.py`](derive.py) refuses a source that does not match and never
replaces an existing output. A change to a setup or to the derivation is
accepted only with a new release derived from it.

## What each image carries

Each setup script holds its own policy in its header and body:

- [`setup/kde`](setup/kde): device groups, getty masks and SDDM autologin into
  Plasma, and the session defaults. The macOS KDE launcher also runs it at boot
  on the unified image and its sealed ext4 derivatives, which predate it.
- [`setup/phrog`](setup/phrog): device groups and compositor defaults for Phosh
  behind greetd and Phrog.
- [`setup/postmarketos`](setup/postmarketos): udev classification and device
  access for the Kernarium and VNC devices under UML.

The KDE image also carries the [Virtio video VA-API
driver](../../video/va-v4l2/README.md): the derivation installs the driver it is
given with `--driver`, the gbm module it is given with `--gbm-module`, and the
`/etc/chromium.d` file that makes Chromium use them, then checks that both
resolve every library the image has. Build them on arm64 Debian 13, whose libva
and libdrm the image carries, with `cmake --preset guest` in
`tools/video/va-v4l2`; the receipt records their digests.

The derivation also removes the machine ID each source was built with, so every
guest generates its own at first boot and keeps it in its writable root.

Each image keeps its own init, network, resolver, accounts and session;
Kernarium boots it through the generic per-run `/init` into `/sbin/init`
([Kernarium/README.md](../../../Kernarium/README.md#guest-boot)). The accepted
KDE image is the one Kernarium selects while its settings name none
([Kernarium/README.md](../../../Kernarium/README.md#guest-storage)), and
`Kernarium/scripts/run-ios-mac.sh` stages it by default from
`.build/rootfs/<release>/`.

## Deriving

The derivation runs as root on arm64 Linux with erofs-utils and writes the new
image with the recorded compressor beside a receipt naming the source, the
setup and the output:

```sh
sudo tools/rootfs/desktop/derive.py --image kde --driver /path/to/uml_drv_video.so \
  --gbm-module /path/to/virtio_gpu_gbm.so \
  --source /path/to/rootfs.erofs --output /path/to/kde-<date>-r<n>.erofs
```

Publish a new release under a new name and record it in `images.json`; never
replace an accepted image or the source it came from. A new KDE release also
renames Kernarium's default image, `RootfsImage.defaultFilename` in
`Kernarium/Kernarium/EmulatorConfiguration.swift`, its tests and the
Kernarium README; `tests/tooling/rootfs/test_desktop_images.py` holds them
to `images.json`.
