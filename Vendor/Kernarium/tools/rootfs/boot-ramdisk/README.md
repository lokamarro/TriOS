# Guest boot ramdisk

Linux guests boot through a throwaway initramfs instead of a startup script
typed onto the kernel command line or edited into the image. This directory
owns that contract: the accepted base archives, the generic per-run `/init`
and the parameters it reads.

## Archive layout

The initrd is `pad4(base) || segment`:

- the **base** is the accepted `uml-boot-base-<arch>.cpio.gz` for the guest's
  userspace architecture, built by the
  [starter producer](../starter/README.md): static BusyBox, e2fsprogs and a
  stub `/init` that prints `UML_BOOT_BASE_ERROR:` and exits, so a boot without a
  per-run segment stops;
- the **segment** is an uncompressed newc archive that `UML::BootInitramfs`
  writes for each run: `/init`, `/boot.env` and any `/payload/...` files.

The kernel unpacks every part into the same ramfs root, so a segment file
replaces the base file of the same name. A segment must follow the base padded
to four bytes, or the kernel drops it as an invalid compressed archive. The
whole initrd sits in boot memory while it unpacks, and one larger than the free
boot memory panics the guest, so payloads stay small.

## Per-run `/init`

[`init`](init) is the default segment `/init`. It mounts devtmpfs, proc and
sysfs, mounts the root image at `/newroot`, installs `/payload` into it, and
`switch_root`s into the entry as PID 1. It prints `UML_BOOT_SWITCH_ROOT:<entry>`
on success and `UML_BOOT_ERROR:<reason>` on any failure, which stops the guest.

`/boot.env` holds `KEY=VALUE` lines (`UML_BOOT_ROOT`, `UML_BOOT_ROOT_TYPE`,
`UML_BOOT_UPPER`, `UML_BOOT_UPPER_DEVICE`, `UML_BOOT_GROW`, `UML_BOOT_ENTRY`);
an unknown key is an error and `init` owns the values and defaults. The root is
mounted one of three ways: `tmpfs` (the default) puts a throwaway tmpfs
overlay above the read-only image, which Kernarium uses for every Linux
desktop; `ext4` is the same overlay on a device `/init` formats afresh, so its
previous contents are lost; `none` makes the image itself the read-write root,
as a starter disk or a one-image machine uses it.
[`../starter/boot.env`](../starter/boot.env) selects `none` for starter disks.

A payload replaces a file the image carries at the same path. A payload
directory must not pass through a symlink in the image: images with a merged
`/usr` link `/lib` to `usr/lib`, so a payload names `/usr/lib/...` instead.

## Harness session

[`session`](session) prepares a disposable run on an image that does not
configure one itself. A launcher installs it as a payload at
`/usr/local/libexec/uml-session` with its parameters at
`/etc/uml-session.env` (`UML_SESSION_*`, parsed and validated by the script)
and names it as `UML_BOOT_ENTRY`. It runs as PID 1 under the image's own shell,
prepares what the parameters ask for, prints `UML_SESSION_READY:<entry>` and
replaces itself with the entry; any failure prints `UML_SESSION_ERROR:<reason>`
and ends the run. An entry other than `/bin/sh` and `systemd` is a workload,
such as a benchmark script: the session runs it as a child, prints
`UML_SESSION_EXIT:<status>`, syncs and powers the guest off.

The session mounts over `/tmp` and `/run`, so a payload a session run reads
lives elsewhere, conventionally under `/usr/local/libexec`. The integration and
performance harness (`tests/integration/support/guest.py`) boots every guest
this way, the GPU runner (`tests/integration/gpu/run_gpu_guest.py`) boots with
`systemd`, and `tools/boot-overlay-systemd.sh` boots an EROFS image
interactively with the fixed parameters in [`overlay-systemd/`](overlay-systemd).
The KDE desktop policy is [`../desktop/setup/kde`](../desktop/setup/kde), baked
into the KDE image by the [desktop derivation](../desktop/README.md); for
images without it, `tools/boot_kde_vnc.py` installs that file as a payload.

## Launchers

`uml-darwin` embeds the accepted base for its kernel slice's userspace
architecture and the default `init`, writes the initrd into a private
temporary directory, and removes it when the run ends.

- `--guest-init=<host file>` replaces `/init`;
- `--guest-file=<archive path>=<host file>` adds a file, such as `/boot.env`
  or `/payload/usr/local/bin/tool`.

Without either option the launcher still boots its base and the default init
unless the command line names `init=` or `initrd=`, which leaves the whole boot
to the caller; `initrd=` together with a `--guest-*` option is an error. The
Android launcher boots Android's own ramdisk and has none of this.
`uml-initramfs` writes the same segment, or with `--base` the whole initrd, for
a host that boots the guest elsewhere. Kernarium's Linux runtime embeds the base
and the default `init` the same way
([Kernarium/README.md](../../../Kernarium/README.md#guest-boot)).

## Accepting a base

`accepted-<arch>.env` names the release, file, size and SHA-256 of each base.
Builds read the archive from `.build/rootfs/<release>/<file>`, or from
`UML_BOOT_BASE_ARCHIVE`, and refuse one that does not match. Before changing a
descriptor, check the new archive:

```sh
tools/rootfs/boot-ramdisk/verify_base.py --descriptor accepted-aarch64.env \
  --archive /path/to/uml-boot-base-aarch64.cpio.gz
```

Publish a new release under a new name; never replace an accepted one. The
unit test `boot-initramfs-archive`, the tooling tests `rootfs-boot-base-contract`
and `rootfs-initramfs-host-tool` and the integration test
`guest-boot-initramfs` cover this contract;
[docs/testing.md](../../../docs/testing.md) runs the integration runner on the
x86_64 slice.
