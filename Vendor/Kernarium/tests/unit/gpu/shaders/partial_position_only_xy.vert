// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
// The same triangle with nothing written to z or w at all.
void main()
{
    const vec2 corner[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position.xy = corner[gl_VertexIndex];
}
