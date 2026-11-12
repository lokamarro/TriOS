# Android guest storage

`uml_android_boot` (`UML::AndroidBoot`) is the host side of an Android guest's
disks. An Android guest has exactly two block devices: the release's read-only
**system disk**, attached as `ubd0r`, and the instance's writable **data
disk**, attached as `ubd1`. The producer's side of the contract, the disk
policy and the layout file, is in the
[Android README](../../../android/lineage-24/README.md#static-disk-and-boot-contract);
this README is what the host guarantees about those files. The macOS launcher
(`darwin/app/macos/android17_main.cpp`) and the Kernarium runtime
(`Kernarium/Runtime`) call the library and nothing else touches a guest's disks.
The library also owns the launcher-set display density and the hardware timeout
multiplier rule (3 from six vCPUs, 5 below), and it refuses a release argument
that sets either, apart from the legacy multiplier `=5`, which it drops.

## The layout file

`readLayout` and `parseLayout` read a release's `guest/partition-layout.json`
(schema 2) with a strict parser that applies the same rules as
`tools/ubd-disk.py`, which writes the file. The host reads the layout and
nothing else about it, so a release changes a size or an offset without a host
change, and a layout the host cannot parse is a release it cannot boot.

## Partition tables

`buildGpt` builds all of a disk's metadata from a size and a partition list.
The same function creates a data disk and checks both disks: `verifyGpt`
compares the disk's metadata byte for byte with what the layout implies, so a
disk another tool touched is not a disk of this layout. The unit test requires
the library to agree with `tools/ubd-disk.py` to the byte. Payload bytes are
never read at boot; the installer verified them.

## Data disk

`createDataDisk`, `growDataDisk` and `prepareDataDisk` write the instance's
disk; `validateDataDisk` checks it.

- A disk is a sparse file of a whole number of GiB within the template's range
  whose tables are exactly the template's for that size. Only the tables are
  written, so any capacity costs the same, and the guest formats `metadata` and
  `userdata` itself and grows its filesystem at the next boot.
- A disk is created beside its path and linked into place, so it never replaces
  a file and a failure leaves nothing behind. A disk only grows, on an APFS
  clone renamed over the disk (a failure leaves the old disk); a volume without
  clones grows it in place, where a crash can leave a disk that fails
  validation. The grow is refused while another process, or another open file
  description of this one, holds a lock on the disk, as a running guest does.
- `prepareDataDisk` is the single entry point of both products: create when
  absent, grow when asked for more, leave it otherwise, refuse less.

## Exactly two disks

`Storage` validates a release root and a data disk and is the only source of a
guest's disk arguments (`kernelArguments`). First-stage init takes a partition
by name from any Android GPT disk it finds and reboots when a name that matters
is missing, so a third disk, or a second instance's, must never be attached. The
library therefore refuses:

- a kernel argument from the host's command line or from the release's
  `kernel-command-line.txt` that names a block device or a ramdisk (`ubd*`,
  `initrd*`, `root=`, `rootfstype=`);
- a release command line without exactly
  `androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1`;
- a system disk that is not exactly the layout's, for example the previous
  single disk; and
- a data disk that is not exactly a data disk of the layout, or a path a UBD
  argument cannot carry.

Both disks are checked again at each boot, because a guest that restarts boots
on whatever the files are by then.

## Boot selection

The boot ramdisks are read in place from the system disk's `init_boot` and
`recovery` partitions. The bootloader message of the data disk's `misc`
partition picks the boot in automatic mode: `boot-recovery` selects recovery.
There is no bootloader, so a restart clears the one-shot `bootonce-bootloader`
request that `reboot bootloader` leaves, and every restart selects its ramdisk
in automatic mode whatever the first boot was given.
