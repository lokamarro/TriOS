#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the partial gl_Position regression shaders using offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    # -O0 keeps the two separate component stores the test is about; the
    # optimizer would merge them back into one vec4 store.
    generate("generate_partial_position.py", __doc__, "partial_position_spirv.hpp", [
        Shader("pp_vertex_split", "partial_position.vert"),
        Shader("pp_vertex_xy", "partial_position_only_xy.vert"),
        Shader("pp_fragment", "partial_position.frag"),
        Shader("pp_fragment_depth", "partial_position_depth.frag"),
    ], optimization="-O0", validator=True)


if __name__ == "__main__":
    main()
