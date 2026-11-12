# Kernel families

Kernel inputs are scoped by source family and version:

- [`mainline-6.18/`](mainline-6.18/README.md) owns the maintained stable Linux
  source identity, generated Darwin patch series, configuration overlays,
  standalone build, and accepted publications: the Linux arm64 and x86_64
  kernels, and the Android kernel built from the same tree. Every guest page is
  4 KiB.
- [`common/`](common/README.md) owns only contracts deliberately shared between
  families, including the kernel configuration policy both overlays extend.
- [`CMakeLists.txt`](CMakeLists.txt) imports accepted local artifacts for Apple
  products without reading a source checkout or invoking Kbuild.

The Android product (LineageOS 24) and its rootfs release are independently
owned by [`../android/lineage-24`](../android/lineage-24/README.md). Kernel and
rootfs release selection must remain independent.

Maintained kernel changes are local to this project. Generated patch series,
not replay checkouts, are the outer repository's source of truth.
