#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate TES transform-feedback shaders using an explicit offline compiler."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    variants = [("vert", "vertex", 0, 0, 0), ("tesc", "control", 0, 0, 0),
                ("frag", "fragment", 0, 0, 0), ("frag", "constant_fragment", 0, 0, 0)]
    for primitive, name in enumerate(["points", "lines", "triangles", "quads"]):
        for clockwise in ([False, True] if primitive >= 2 else [False]):
            for capture in [False, True]:
                variant = name + ("_cw" if clockwise else "") + ("_capture" if capture else "")
                variants.append(("tese", variant, primitive, int(capture), int(clockwise)))
        variants.append(("tese", name + "_packed_capture", primitive, 1, 0))
    shaders = [
        Shader(name, f"tessellation_feedback.{stage}",
               [*(["-DCONSTANT_COLOR=1"] if name == "constant_fragment" else []),
                f"-DPRIMITIVE={primitive}", f"-DCAPTURE={capture}",
                f"-DPACKED={int(name.endswith('_packed_capture'))}",
                f"-DCLOCKWISE={clockwise}"])
        for stage, name, primitive, capture, clockwise in variants
    ]
    generate("generate_tessellation_feedback.py", __doc__,
             "tessellation_feedback_spirv.hpp", shaders,
             namespace="tessellation_feedback")


if __name__ == "__main__":
    main()
