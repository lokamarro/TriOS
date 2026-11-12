#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the GS invocation/stream regression shaders using offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def line_output(words: list[int]) -> None:
    """Change the GLSL frontend's points output to valid Vulkan multistream lines."""
    cursor = 5
    changed = 0
    while cursor < len(words):
        size, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if not size or cursor + size > len(words):
            raise ValueError("invalid SPIR-V instruction")
        if opcode == 16 and words[cursor + 2] == 27:  # OpExecutionMode OutputPoints
            words[cursor + 2] = 28  # OutputLineStrip
            changed += 1
        cursor += size
    if changed != 1:
        raise ValueError("expected exactly one OutputPoints execution mode")


def main() -> None:
    shaders = [Shader("gi_vertex", "geometry_invocations.vert"),
               Shader("gi_fragment", "geometry_invocations.frag")]
    for invocations in (2, 4):
        for explicit in (0, 1):
            shaders.append(Shader(f"gi_i{invocations}_end{explicit}",
                                  "geometry_invocations.geom",
                                  [f"-DINVOCATIONS={invocations}",
                                   f"-DEXPLICIT_END={explicit}"],
                                  line_output))
    generate("generate_geometry_invocations.py", __doc__,
             "geometry_invocations_spirv.hpp", shaders, validator=True)


if __name__ == "__main__":
    main()
