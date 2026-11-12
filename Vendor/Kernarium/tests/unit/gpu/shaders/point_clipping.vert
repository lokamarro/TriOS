// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_GOOGLE_include_directive : require
#include "point_clipping_output.glsl"
void main()
{
    write_point(UPSTREAM != 0 ? 16u : 0u);
}
