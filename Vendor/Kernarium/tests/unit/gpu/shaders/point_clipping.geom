// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_GOOGLE_include_directive : require
layout(points) in;
layout(points, max_vertices = 1) out;
in gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
} gl_in[];
#include "point_clipping_output.glsl"
void main()
{
    write_point(0u);
    EmitVertex();
    EndPrimitive();
}
