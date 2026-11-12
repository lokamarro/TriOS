// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_GOOGLE_include_directive : require
layout(triangles, equal_spacing, ccw, point_mode) in;
in gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
} gl_in[];
#include "point_clipping_output.glsl"
void main()
{
    // All three generated points overlap, isolating final-stage clipping.
    write_point(0u);
}
