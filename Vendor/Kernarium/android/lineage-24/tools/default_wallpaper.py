#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The product's default wallpaper: a bounded copy of LineageOS's.

LineageOS ships its default wallpaper as square, nearly detail-free PNGs, one per
density bucket, and the `sw600dp` and `sw720dp` variants of every display wide
enough for a tablet are 1920 x 1920. SystemUI's ImageWallpaper fixes its surface
to the bitmap size, so a 1133 x 687 display then holds a 14 MiB buffer that
SurfaceFlinger scales down into a 1133 x 1133 wallpaper window. The overlay
replaces those two variants with a 1024 x 1024 copy of the same image: the
window, and with it the home screen's parallax range, is sized from the display
and not from the bitmap, so only the buffer shrinks, to 4 MiB.

LineageOS's overlay names the same resource, and where two overlay directories do
that, the one listed earlier in PRODUCT_PACKAGE_OVERLAYS wins (Soong gives the
earlier directory the higher priority). inherit-product expands the inherited list
where the call stands, so the copies live in their own directory,
`overlay/overlay-priority`, which `overlay/lineage_andarium.mk` lists before its
inherit-product calls; the ordinary `overlay/overlay` directory is listed after
them and ranks under LineageOS's and Cuttlefish's.

The wallpaper's colours decide the Monet palette: SystemUI quantizes a 112 x 112 mini
bitmap that `Bitmap.createScaledBitmap(bitmap, 112, 112, false)` takes from the
bitmap with one nearest-neighbour texel per sample, so a resize of the 1920 pixel
image moves the accent colour (Lanczos gives #78ACB9 where LineageOS's own gives
#589EA8) even though no pixel differs by more than a few levels. `generate` therefore
writes, into each texel the 1024 pixel image is sampled at, the value the 1920 pixel
image's own sample reads. The two images give the quantizer the same pixels and so the
same palette; about 0.4% of the texels change, by at most a handful of levels.

`check` verifies the committed files and that order, `generate` rewrites the files
from LineageOS's pinned `drawable-sw600dp-nodpi/default_wallpaper.png` with
ImageMagick. The verifier of the built target files uses `png_size` on the
framework runtime overlay, which is where the order shows.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# The overlay directories whose default_wallpaper.png this product replaces. The platform
# prefers the sw720dp variant to the sw600dp one wherever it applies, so both are needed.
VARIANT_DIRECTORIES = ("drawable-sw600dp-nodpi", "drawable-sw720dp-nodpi")
OVERLAY_RESOURCES = "overlay/overlay-priority/frameworks/base/core/res/res"
# The product makefile, and the two PRODUCT_PACKAGE_OVERLAYS entries whose order matters.
PRODUCT_MAKEFILE = "overlay/lineage_andarium.mk"
PRIORITY_OVERLAY = "device/kernarium/andarium/overlay-priority"
ORDINARY_OVERLAY = "device/kernarium/andarium/overlay"
ORDINARY_RESOURCES = "overlay/overlay/frameworks/base/core/res/res"
FILE_NAME = "default_wallpaper.png"
# No variant of the built framework overlay may be larger than this, on either side.
MAX_SIDE = 1024
# LineageOS's own sw600dp and sw720dp file (the two are identical) that the copy comes from.
SOURCE_SHA256 = "ae3e5e7754300922dd499e969cf2fa892b2f0a429502ccf750289338cc18374d"
SOURCE_SIDE = 1920
# SystemUI's mini bitmap is MINI_SIDE x MINI_SIDE. Sample k of an axis reads source texel
# floor((k + 0.5) * side / MINI_SIDE); where that position is an exact integer the guest's
# float arithmetic takes the texel before it for the 1920 pixel image (read on a guest from an
# index image, all 15 such samples of both axes), and either one for the 1024 pixel image.
MINI_SIDE = 112
# The committed copy. A different ImageMagick can write other bytes for the same pixels, so a
# regeneration prints the new digest for this line.
OUTPUT_SHA256 = "7188fa413cab51c6da81be488a2b2bd8329aae3f6a0d66eed479d43e1d6d99fc"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class WallpaperError(Exception):
    pass


def png_size(data: bytes) -> tuple[int, int, int, int]:
    """Width, height, bit depth and colour type of a PNG, read from its IHDR chunk."""
    if data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR" or len(data) < 29:
        raise WallpaperError("not a PNG with a leading IHDR chunk")
    width, height, depth, color = struct.unpack(">IIBB", data[16:26])
    return width, height, depth, color


def check_files(android_root: Path) -> None:
    """The priority overlay holds the same bounded 8-bit RGB copy under both variants."""
    resources = android_root / OVERLAY_RESOURCES
    payloads = []
    for directory in VARIANT_DIRECTORIES:
        path = resources / directory / FILE_NAME
        if not path.is_file():
            raise WallpaperError(f"the overlay has no {directory}/{FILE_NAME}")
        data = path.read_bytes()
        try:
            width, height, depth, color = png_size(data)
        except WallpaperError as error:
            raise WallpaperError(f"{directory}/{FILE_NAME} is {error}") from None
        if width != height or width > MAX_SIDE:
            raise WallpaperError(
                f"{directory}/{FILE_NAME} is {width}x{height}: the default wallpaper is a square "
                f"of at most {MAX_SIDE} pixels"
            )
        if depth != 8 or color != 2:
            raise WallpaperError(
                f"{directory}/{FILE_NAME} must be 8-bit RGB without alpha (bit depth {depth}, "
                f"colour type {color})"
            )
        payloads.append(data)
    if len(set(payloads)) != 1:
        raise WallpaperError("the sw600dp and sw720dp default wallpapers differ")
    digest = hashlib.sha256(payloads[0]).hexdigest()
    if digest != OUTPUT_SHA256:
        raise WallpaperError(
            f"the default wallpaper digest is {digest}, not {OUTPUT_SHA256}: regenerate it with "
            "`tools/default_wallpaper.py generate` and record the new digest"
        )


def check_precedence(android_root: Path) -> None:
    """The priority overlay is listed before the first inherit-product call, the ordinary one after it.

    The built framework overlay is the final proof (the verifier and the canary read it), but this
    fails in the source tree, before a build, when the directory that has to win ranks under
    LineageOS's."""
    text = (android_root / PRODUCT_MAKEFILE).read_text(encoding="utf-8")
    lines = [re.sub(r"#.*", "", line).strip() for line in text.splitlines()]

    def positions(entry: str) -> list[int]:
        pattern = re.compile(r"^PRODUCT_PACKAGE_OVERLAYS\s*\+=\s*" + re.escape(entry) + r"\s*$")
        return [number for number, line in enumerate(lines) if pattern.match(line)]

    inherits = [number for number, line in enumerate(lines) if line.startswith("$(call inherit-product,")]
    if not inherits:
        raise WallpaperError(f"{PRODUCT_MAKEFILE} has no inherit-product call to order against")
    priority = positions(PRIORITY_OVERLAY)
    if len(priority) != 1:
        raise WallpaperError(f"{PRODUCT_MAKEFILE} must add {PRIORITY_OVERLAY} to PRODUCT_PACKAGE_OVERLAYS exactly once")
    if priority[0] > inherits[0]:
        raise WallpaperError(
            f"{PRODUCT_MAKEFILE} adds {PRIORITY_OVERLAY} after its first inherit-product call: the "
            "directory then ranks under LineageOS's overlay and its default wallpaper is not used"
        )
    ordinary = positions(ORDINARY_OVERLAY)
    if len(ordinary) != 1 or ordinary[0] < inherits[-1]:
        raise WallpaperError(
            f"{PRODUCT_MAKEFILE} must add {ORDINARY_OVERLAY} once, after its inherit-product calls"
        )
    for directory in VARIANT_DIRECTORIES:
        if (android_root / ORDINARY_RESOURCES / directory / FILE_NAME).exists():
            raise WallpaperError(
                f"{directory}/{FILE_NAME} is also in the ordinary overlay, which ranks under "
                "LineageOS's: keep it only in the priority overlay"
            )


def sample_texels(side: int, sample: int) -> tuple[int, int]:
    """The (first, last) texel columns, or rows, that SystemUI's mini bitmap sample `sample` can read."""
    numerator = (2 * sample + 1) * side
    position = numerator // (2 * MINI_SIDE)
    if numerator % (2 * MINI_SIDE) == 0:
        return position - 1, position
    return position, position


def pin_samples(output: bytearray, output_side: int, source: bytes, source_side: int) -> int:
    """Copy the source image's mini bitmap samples into the output image's sample texels.

    Both are packed 8-bit RGB. Returns the number of texel values that changed."""
    changed = 0
    for row in range(MINI_SIDE):
        source_row = sample_texels(source_side, row)[0]
        for column in range(MINI_SIDE):
            source_column = sample_texels(source_side, column)[0]
            start = (source_row * source_side + source_column) * 3
            value = source[start:start + 3]
            for y in range(sample_texels(output_side, row)[0], sample_texels(output_side, row)[1] + 1):
                for x in range(sample_texels(output_side, column)[0], sample_texels(output_side, column)[1] + 1):
                    offset = (y * output_side + x) * 3
                    if output[offset:offset + 3] != value:
                        changed += 1
                        output[offset:offset + 3] = value
    return changed


def raw_rgb(executable: str, path: Path, side: int) -> bytes:
    data = subprocess.run([executable, str(path), "-depth", "8", "-alpha", "off", "rgb:-"],
                          check=True, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE).stdout
    if len(data) != side * side * 3:
        raise WallpaperError(f"{path} does not hold {side}x{side} 8-bit RGB pixels")
    return data


def generate(android_root: Path, source: Path, magick: str) -> str:
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256:
        raise WallpaperError(
            f"{source} is not LineageOS's default wallpaper ({digest}, expected {SOURCE_SHA256}); "
            "a new Lineage pin needs this tool's SOURCE_SHA256 and OUTPUT_SHA256 reviewed"
        )
    width, height, _, _ = png_size(data)
    if (width, height) != (SOURCE_SIDE, SOURCE_SIDE):
        raise WallpaperError(f"{source} is {width}x{height}, not {SOURCE_SIDE}x{SOURCE_SIDE}")
    executable = shutil.which(magick)
    if executable is None:
        raise WallpaperError(f"{magick} is not installed")
    resources = android_root / OVERLAY_RESOURCES
    with tempfile.TemporaryDirectory(prefix="default-wallpaper-") as temporary:
        resized = Path(temporary) / "resized.png"
        output = Path(temporary) / FILE_NAME
        subprocess.run(
            [executable, str(source), "-filter", "Lanczos", "-resize", f"{MAX_SIDE}x{MAX_SIDE}!",
             "-strip", "-depth", "8", f"PNG24:{resized}"],
            check=True, stdin=subprocess.DEVNULL,
        )
        pixels = bytearray(raw_rgb(executable, resized, MAX_SIDE))
        pin_samples(pixels, MAX_SIDE, raw_rgb(executable, source, SOURCE_SIDE), SOURCE_SIDE)
        subprocess.run(
            [executable, "-size", f"{MAX_SIDE}x{MAX_SIDE}", "-depth", "8", "rgb:-", "-strip",
             "-define", "png:compression-level=9", "-define", "png:exclude-chunk=all", f"PNG24:{output}"],
            check=True, input=bytes(pixels),
        )
        written = output.read_bytes()
        for directory in VARIANT_DIRECTORIES:
            target = resources / directory
            target.mkdir(parents=True, exist_ok=True)
            (target / FILE_NAME).write_bytes(written)
    return hashlib.sha256(written).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("action", choices=("check", "generate"), nargs="?", default="check")
    parser.add_argument("--android-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--source", type=Path,
                        help="LineageOS's drawable-sw600dp-nodpi/default_wallpaper.png (generate)")
    parser.add_argument("--magick", default="magick")
    arguments = parser.parse_args()
    try:
        if arguments.action == "generate":
            if arguments.source is None:
                raise WallpaperError("generate needs --source")
            print(f"wrote the default wallpaper, sha256 {generate(arguments.android_root, arguments.source, arguments.magick)}")
        else:
            check_files(arguments.android_root)
            check_precedence(arguments.android_root)
            print("default wallpaper overlay: ok")
    except (WallpaperError, subprocess.CalledProcessError, OSError) as error:
        print(f"default wallpaper: {error}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
