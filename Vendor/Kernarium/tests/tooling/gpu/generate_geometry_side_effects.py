#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate geometry side-effect regression shaders using an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    shaders = [Shader("gs_vertex", "geometry_side_effects.vert"),
               Shader("gs_fragment", "geometry_side_effects.frag")]
    for name, shape, used in (("two_strips", 0, 1), ("one_strip", 1, 1),
                              ("no_output", 2, 1), ("unused", 0, 0)):
        for capture in (0, 1):
            shaders.append(Shader(f"gs_{name}_{capture}", "geometry_side_effects.geom",
                                  [f"-DSHAPE={shape}", f"-DUSE_RESULT={used}",
                                   f"-DCAPTURE={capture}"]))
    generate("generate_geometry_side_effects.py", __doc__,
             "geometry_side_effects_spirv.hpp", shaders)


if __name__ == "__main__":
    main()
