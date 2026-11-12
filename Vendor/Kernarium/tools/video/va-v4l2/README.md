# VA-API backend over the Virtio video decoder and encoder

`uml_drv_video.so` is a libva backend driver that lets a VA-API client in a
Linux guest decode and encode video through the [Virtio video
device](../../../darwin/device/video/README.md), whose V4L2 stateful mem2mem
nodes the guest kernel publishes as `/dev/videoN`. It exists because Valve's
x86-64 CEF carries only the VA-API decode path while the device speaks V4L2,
because Debian's Chromium uses VA-API for hardware encode as well as decode, and
because no packaged libva backend drives a stateful codec.

It is a guest component: it is built natively for the guest it runs in, against
that guest's own libva and libdrm, and is never part of the Apple host build.
Any VA-API client works with it; the driver implements the public interface and
nothing else (`LIBVA_DRIVER_NAME=uml`).

## Contracts

- **Offered.** VP9 Profile 0 decode, H.264 and HEVC encode (the profiles and
  rate-control modes the encoder node offers), and `VAProfileNone` video
  processing (NV12/ARGB conversion, cropping, scaling). H.264 and HEVC *decode*
  are not advertised: a stateful decoder needs the original byte stream, which
  a VA-API submission no longer carries, and advertising them would make a
  client pick this driver and fail. Everything else answers
  `VA_STATUS_ERROR_UNIMPLEMENTED`; nothing aborts, because this code runs inside
  a browser's GPU process.
- **Synchronous decode.** `vaEndPicture` waits for the picture, because the
  device decodes synchronously and Chromium's wrapper never calls
  `vaSyncSurface` after a decode. Hidden VP9 frames are shown to the decoder
  immediately (`UML_VA_V4L2_HIDDEN`), so a client that later displays one by
  submitting nothing finds its surface.
- **One object per exported surface.** A surface is one buffer holding every
  plane, because Chromium's dma-buf export asserts exactly one object. A
  descriptor a client *imports* may name several objects (the gbm module below
  allocates one per plane); such a surface serves as an encode input only.
- **dma-buf synchronisation.** Every CPU access to a surface a client imported,
  or to a GBM surface the driver allocated, is bracketed with
  `DMA_BUF_IOCTL_SYNC`. A mapping returned by `vaDeriveImage` stays the
  client's to bracket.
- **Packed RGB decode.** The decoder writes packed RGB (`AR24`) by default,
  because a browser converts every frame to RGB and converting NV12 in the guest
  costs more than copying the larger frame
  ([`optimization-history.md`](../../../docs/optimization-history.md)). The
  choice is made once at initialisation from what the device offers;
  `UML_VA_V4L2_RGB=off` keeps the device's own planes. With
  `UML_VA_V4L2_EXPORT=argb`, `vaExportSurfaceHandle` hands over the RGB shadow
  as one ARGB plane instead of NV12, which a guest Mesa that carries no plane
  offset can sample.
- **Lent buffers.** On a capture queue that accepts `V4L2_MEMORY_DMABUF` the
  device decodes straight into the driver's own surfaces
  (`UML_VA_V4L2_CAPTURE`). Only single-plane capture formats can be lent.
- **Encode.** A profile or rate-control mode is listed only when the encoder
  node offers it. One VA context runs one encode session, started with the
  first picture because stream controls cannot change while it runs; B-frames,
  multiple slices and temporal layers are refused, not approximated.
  `vaSyncSurface` and `vaMapBuffer` wait up to four seconds for a frame.
- **Sandboxed processes.** A browser initialises VA-API before it engages its
  sandbox, and a sandboxed process cannot open device nodes, so the driver
  opens a pool of V4L2 descriptors at the first initialisation in a process
  (`UML_VA_V4L2_POOL`). With the stock empty in a sandbox, `vaCreateContext`
  answers `VA_STATUS_ERROR_MAX_NUM_EXCEEDED` and the client falls back to
  software.
- **Serialisation.** Every driver entry point that reads or changes driver state
  takes the driver's lock; keep it so when adding one.
- **Configuration.** The driver reads `UML_VA_V4L2_*` and `UML_VA_DRM_DEVICE`
  variables (device nodes, pool size, surface backing store, RGB/export/capture
  policy, and `LOG`, `TRACE`, `REPORT`, `DUMP` diagnostics); `driver.cpp`,
  `entry.cpp` and `v4l2_pool.cpp` own their values.

The mechanics of packet building, buffer lending and encode parameter mapping
are in the comments of `driver.hpp`, `v4l2_decoder.hpp`, `encode.hpp` and
`encode_map.hpp`.

## Building and testing

```sh
cmake --preset guest && cmake --build --preset guest
./build/uml_va_v4l2_tests
LIBVA_DRIVER_NAME=uml LIBVA_DRIVERS_PATH=$PWD/build \
  ./build/va_decode_probe movie.ivf --out - | md5sum
```

The unit test covers VP9 headers, superframes, pixel paths, the pool, and encode
against an encoder node held in memory. `build/va_decode_probe` and
`build/va_encode_probe` are the guest-side qualification clients: they drive
libva the way Chromium's VA-API decoder and encoder do, and the decode probe's
planar I420 output compares byte for byte with a reference decode.
`tests/probe_selftest.sh build` and `tests/ffmpeg_selftest.sh build` run them
through libva's own loader against `fake_drv_video`, the same driver over the
in-memory encoder node, so they work on a host without a Virtio video device.
The probes are Linux guest programs; the unit test and self-tests also run on
any Linux build host.

## The gbm module: planar buffers for Debian's libgbm

Chromium allocates the frames it hands to a hardware encoder through gbm, and
Debian's libgbm cannot allocate any planar format, so NV12, P010 and the
three-plane layouts fail with `EINVAL`. `virtio_gpu_gbm.so` ([`gbm/`](gbm/)) is
the backend module that fixes this for the virtio-gpu device. libgbm loads a
backend module named after the DRM driver of the node it is given, so
installing the file as `/usr/lib/<triplet>/gbm/virtio_gpu_gbm.so` is the whole
configuration; `GBM_BACKEND=dri` turns it off for a process.

- **It adds planar buffers and nothing else.** It loads Mesa's own DRI backend
  and replaces only the entries of that device's function table that a planar
  buffer needs; every colour-format buffer, scanout, gbm surface and EGL window
  is Mesa's. When it cannot load Mesa's backend, or Mesa already allocates NV12,
  libgbm falls back to Mesa.
- **A planar buffer is one buffer object per plane**, each in the format the
  plane really has, with its own dma-buf at offset 0, linear, which is how
  Mesa describes a planar image to the GPU and what Chromium imports plane by
  plane. `gbm_bo_map` and `gbm_bo_write` of a whole planar image fail with
  `ENOSYS`; a client maps each plane's dma-buf, and a planar buffer reaches EGL
  through its plane descriptors.
- **Only 64-bit.** It is built for the guest's native architecture, not for the
  32-bit libgbm of the x86-64 image.
- **It depends on libgbm's backend interface version 1**, which is frozen.
  [`gbm/abi.hpp`](gbm/abi.hpp) declares that layout because Debian's libgbm-dev
  does not install the header.

What a client may do with the plane descriptors depends on the guest kernel: a
descriptor Mesa exports is read-only unless the kernel exports virtio-gpu
buffers read-write, and what the GPU wrote reaches the guest's pages only where
the kernel transfers it around a dma-buf CPU access. `tests/gbm_planar_tests.cpp`
(inside `uml_va_v4l2_tests`) and the guest-side `gbm_planar_probe` cover the
module.

## In the Debian desktop images

The starter Plasma image, the KDE image and the x86-64 desktop image carry
Debian's Chromium, the driver and [`chromium.d/kernarium-video`](chromium.d/kernarium-video),
which `/usr/bin/chromium` sources at start. The file names the driver (libva
would otherwise map the virtio-gpu node to Mesa's driver), points Chromium at
the guest's render node (not a PCI device, so Chromium finds none itself and
leaves VA-API off), and enables Chromium's hardware encode. VP9 decodes in
hardware; H.264 and HEVC decode stay in software because the driver does not
offer them. Chromium's encode is H.264 only.

- **Exported decode surfaces are ARGB.** Chromium hands each decoded surface to
  the compositor, and an NV12 descriptor (one object, chroma at an offset)
  shows black or wrong chroma on a guest Mesa that cannot import a plane at an
  offset, so the file sets `UML_VA_V4L2_EXPORT=argb`.
- **Encode frames come from the gbm module and import plane by plane.**
  Chromium imports one descriptor per plane only for a driver it recognises by
  vendor string, so the driver's vendor string begins with `Mesa Gallium
  driver`. It must stay thread-safe for that reason (see Serialisation).
- **GPU-painted frames are current for the CPU only on a suitable guest
  kernel**: one that exports virtio-gpu dma-bufs read-write and transfers the
  resource around a dma-buf CPU access. Without it a GPU-painted frame encodes
  black.

[`tools/rootfs/producer.sh`](../../rootfs/producer.sh) builds the driver inside
an image's own root (`producer_install_uml_va_driver`), against the libva and
libdrm the image ships, runs the unit test and the probe self-test there,
installs both files with `cmake --install --prefix /usr` and removes the build
tools; the starter and x86-64 producers call it, and the
[desktop derivation](../../rootfs/desktop/README.md) installs a driver built the
same way into the KDE image.

## In the Steam x86-64 image

[`tools/rootfs/steam-x86_64`](../../rootfs/steam-x86_64/README.md) compiles this
driver in its builder root, runs the unit test there, and installs the module as
`/usr/lib/x86_64-linux-gnu/dri/uml_drv_video.so`. The gaming session exports
`LIBVA_DRIVER_NAME=uml` and `KERNARIUM_CEF_VAAPI=1` when the module and
`/dev/video-dec0` are both present, and the session supervisor then starts the
client with Valve's `-cef-enable-vaapi`, including its
`VaapiIgnoreDriverChecks` feature, without which Chromium accepts only the
driver vendor strings it knows.
