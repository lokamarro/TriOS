#!/usr/bin/env python3
"""Build-local correction: ARB_shader_draw_parameters is valid before GLSL 440.

Keep upstream sources pinned. Extension gating remains in the parser; this
only installs the ARB builtins and their semantics for desktop GLSL >= 140.
"""
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
changes = [
    ('if (version >= 440) {\n            stageBuiltins[EShLangVertex].append(\n                "in int gl_BaseVertexARB;"',
     'if (version >= 140) {\n            stageBuiltins[EShLangVertex].append(\n                "in int gl_BaseVertexARB;"'),
    ('if (version >= 440) {\n                symbolTable.setVariableExtensions("gl_BaseVertexARB",',
     'if (version >= 140) {\n                symbolTable.setVariableExtensions("gl_BaseVertexARB",'),
]
for old, new in changes:
    if source.count(old) != 1:
        raise SystemExit('Pinned glslang builtins changed; review ARB_shader_draw_parameters integration')
    source = source.replace(old, new)
pathlib.Path(sys.argv[2]).write_text(source)
