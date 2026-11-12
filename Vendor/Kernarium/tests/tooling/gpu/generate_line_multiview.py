#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the embedded line-multiview SPIR-V using an offline glslc."""

__import__("sys").dont_write_bytecode = True

from spirv_header import Shader, generate


def main() -> None:
    generate("generate_line_multiview.py", __doc__, "line_multiview_spirv.hpp", [
        Shader("line_multiview_vert_spirv", "line_multiview.vert"),
        Shader("line_multiview_layer_vert_spirv", "line_multiview.vert",
               ["-DWRITE_LAYER"]),
        Shader("line_multiview_frag_spirv", "line_multiview.frag"),
    ])


if __name__ == "__main__":
    main()
