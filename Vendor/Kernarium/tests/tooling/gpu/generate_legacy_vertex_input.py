#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate legacy vertex-input regression shaders using an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    generate("generate_legacy_vertex_input.py", __doc__,
             "legacy_vertex_input_spirv.hpp", [
                 Shader("legacy_vertex", "legacy_vertex_input.vert", ["-DCAPTURE=0"]),
                 Shader("legacy_capture", "legacy_vertex_input.vert", ["-DCAPTURE=1"]),
                 Shader("legacy_fragment", "legacy_vertex_input.frag"),
             ])


if __name__ == "__main__":
    main()
