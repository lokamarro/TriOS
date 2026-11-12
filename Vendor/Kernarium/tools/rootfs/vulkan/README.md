# Patched Vulkan guest userspace

This directory owns the source inputs for the separately authorized Vulkan
rootfs derivative. It does not alter `rootfs.erofs` or the sealed GL/GLES
derivative. The image is accepted only when its checked receipt, read-only
attachment, CTS identity, and platform gates all pass.

The host backend is Venus over KosmicKrisp. The guest uses an ordinary Vulkan
ICD manifest rather than a portability-subset driver. Guest and renderer
codecs are generated offline into build directories from the same patched
Venus protocol and pinned SDK registry; generated codecs, SDK headers, and
registry copies are not carried in the downstream patch series. The Android
Vulkan HAL's port is the one carrier of a generated codec. It pins the protocol
series and Vulkan-Headers trees recorded here, so a change to either needs
that port regenerated, as the
[Android producer](../../../android/lineage-24/README.md#graphics) describes.

The guest Mesa series ([`patches/mesa/series.tsv`](patches/mesa/series.tsv))
is intentionally downstream and carries transport, reporting, and sharing
correctness fixes; each topic's commit message states its contract. How a
guest shares a swapchain image with the renderer is in
[`graphics.md`](../../../docs/graphics.md#vulkan).

Passing this project's receipt-gated CTS campaign demonstrates the tested
stack's result. It is not by itself a Khronos conformance claim; that also
requires the applicable submission, review, and trademark process.

## Layout

| Path | Role |
| --- | --- |
| `sources.tsv` | every upstream commit, tree and release object, plus the generated Mesa, Venus-protocol and CTS series |
| `patches/` | the generated series (`mesa`, `venus-protocol`, `vk-gl-cts`) |
| `exclusions.txt` | the exact removal set applied to a fresh extraction of the base image |
| `seal-in-vm.py`, `verify-receipt.py` | build and verify the image and its receipt |
| `stage-runtime.py`, `install-runtime.py` | build and install the relocatable guest runtime |
| `verify-inputs.py` | the input and series checks |

Change a generated series through a topic checkout and regenerate it with
`tools/rootfs/vulkan/rebase-patches.sh --component NAME --source CHECKOUT`.
The Mesa series is also the lower segment of the host's Mesa series in
[`dependencies/patches/mesa`](../../../dependencies/README.md#apple-gpu-prefix):
export this segment with `--head` naming its last commit, then the host layer
with `dependencies/rebase-patches.sh`. A guest Mesa change therefore also
invalidates the host prefixes.

## Receipts

The accepted CTS images and their receipts name one patched tree per
component, and each component's `series.tsv` records the tree its patches
produce. A series edit that keeps that tree, such as a reworded message or a
folded follow-up, leaves the receipts valid. A series that produces a different
tree invalidates them until new images are sealed and accepted: the CTS runners
refuse the stale images and the `vulkan-guest-rootfs-safety` tooling test fails
with `accepted CTS image is stale`. That is the intended interlock; the way to
clear it is to seal new images, never to relax a verifier. The identities of
the base and derivative images are in `tests/integration/gpu/vulkan_cts_receipt.json`.

The combined Vulkan/Zink/OpenGL derivative (`--include-opengl-cts`) has its own
artifact identity in `tests/integration/gpu/vk_gl_cts_derivative_rootfs.json`
and its own receipt, and never replaces the accepted Vulkan-only image.

## Sealing

The final build, CTS build, image creation and receipt generation run in a
native arm64 Debian VM or, under `fakeroot`, an unprivileged arm64 Debian
container; the receipt names the environment kind. Seal only new stage, image,
and receipt paths; the sealer refuses existing outputs and never mounts or
boots an image writable. `--help` lists every option:

```sh
sudo tools/rootfs/vulkan/seal-in-vm.py --work-root WORK \
  --base-rootfs WORK/input/rootfs.erofs --stage-root NEW-STAGE \
  --output NEW.ext4 --receipt NEW.receipt.json
```

After direct transfer from the VM, verify both the receipt and image:

```sh
tools/rootfs/vulkan/verify-receipt.py \
  --image .build/rootfs/rootfs-vulkan-venus.ext4
```

## Relocatable guest runtime

`stage-runtime.py` produces the guest runtime for one architecture as a
relocatable root plus its own receipt, with no CTS payload and no image
identity, and records its dynamic-dependency closure against a base root of the
distribution the consuming image will use. `install-runtime.py` imports that
pair into an image root: it authenticates every file against the receipt
before the image is touched, removes every other ICD manifest so a software
driver cannot mask a Venus failure, refuses an unresolved dynamic dependency,
and records what it installed in `/usr/share/uml-vulkan/guest-runtime.json`.
`architecture_profile()` in `seal-in-vm.py` owns the layout contract, which
differs between architectures only in the multiarch library directory.
