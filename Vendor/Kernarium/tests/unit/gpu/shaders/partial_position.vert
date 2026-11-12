// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
// gl_Position is a vec4 whose unwritten components are undefined, so writing
// only xy is legal. Metal's [[position]] is float4 regardless, and a driver
// that sizes the output struct from the store emits float2 and fails to build
// the shader library at all.
void main()
{
    const vec2 corner[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position.xy = corner[gl_VertexIndex];
    gl_Position.zw = vec2(0.0, 1.0);
}
