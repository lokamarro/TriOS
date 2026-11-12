// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) in uint value;
layout(location = 0) flat out vec4 color;
#if CAPTURE
layout(location = 1, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 4) out uint captured;
#endif
void main()
{
    float red = value == 0x3f800000u ? 0.25 : value == 65535u ? 0.75 : 0.0;
    color = vec4(red, gl_VertexIndex == 0 ? 0.25 : 0.75, 0.5, 1.0);
    gl_Position = vec4((8.5 + 48.0 * gl_VertexIndex) / 32.0 - 1.0, 0.125, 0.0, 1.0);
#if CAPTURE
    captured = value;
#endif
}
