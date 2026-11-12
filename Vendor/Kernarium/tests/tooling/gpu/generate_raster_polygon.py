#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate polygon rasterization regression shaders using an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    variants = [("vert", "vertex", ["-DCAPTURE=0"]),
                ("vert", "capture", ["-DCAPTURE=1"]),
                ("geom", "geometry", []), ("tesc", "control", []),
                ("tese", "evaluation", []),
                ("frag", "fragment", ["-DPOINT_COORD=1"]),
                ("frag", "facing", ["-DPOINT_COORD=0"]),
                ("frag", "attributes", ["-DATTRIBUTES=1"]),
                ("frag", "early_facing", ["-DEARLY=1"])]
    for shape in range(1, 7):
        for stage, name, defines in list(variants[:5]):
            variants.append((stage, f"{name}_{shape}", [*defines, f"-DSHAPE={shape}"]))
    variants.extend([("vert", "endpoints_vertex", []),
                     ("vert", "endpoints_point_vertex", ["-DPOINT_SIZE_STORAGE=1"]),
                     ("frag", "endpoints_fragment", [])])
    shaders = [
        Shader(f"polygon_{name}",
               ("raster_endpoints" if name.startswith("endpoints_")
                else "raster_polygon") + f".{stage}",
               defines)
        for stage, name, defines in variants
    ]
    generate("generate_raster_polygon.py", __doc__, "raster_polygon_spirv.hpp", shaders)


if __name__ == "__main__":
    main()
