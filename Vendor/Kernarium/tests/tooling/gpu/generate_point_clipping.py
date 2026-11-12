#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate native point-clipping regression shaders with an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    variants = [("vert", "upstream", 1, 0, 0, 1),
                ("tesc", "control", 1, 0, 0, 0),
                ("frag", "fragment", 1, 0, 0, 0)]
    for stage, name in [("vert", "vertex"), ("geom", "geometry"),
                        ("tese", "evaluation")]:
        for suffix, planes, cull, capture in [("", 1, 0, 0), ("_none", 0, 0, 0),
                                              ("_eight", 8, 0, 0), ("_cull", 0, 1, 0),
                                              ("_capture", 1, 0, 1), ("_effects", 1, 0, 0),
                                              ("_partial", 1, 0, 0)]:
            variants.append((stage, name + suffix, planes, cull, capture, 0))
    shaders = [
        Shader(name, f"point_clipping.{stage}",
               [f"-DCLIP_PLANES={planes}", f"-DCULL_PLANE={cull}",
                f"-DCAPTURE={capture}", f"-DUPSTREAM={upstream}",
                f"-DEFFECTS={int(capture or name.endswith('_effects'))}",
                f"-DPARTIAL={int(name.endswith('_partial'))}"])
        for stage, name, planes, cull, capture, upstream in variants
    ]
    generate("generate_point_clipping.py", __doc__, "point_clipping_spirv.hpp",
             shaders, namespace="point_clipping")


if __name__ == "__main__":
    main()
