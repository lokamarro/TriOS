// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 1) flat out uint flat_tag;
layout(location = 2) out float smooth_value;
layout(location = 3) noperspective out float linear_value;
#if CAPTURE
layout(location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0)
flat out uvec4 tag;
#endif
void main()
{
    const vec2 positions[3] = vec2[](vec2(16, 16), vec2(48, 16), vec2(16, 48));
    const float weights[3] = float[](1, 4, 2);
    uint vertex = uint(gl_VertexIndex) % 3u;
    flat_tag = vertex;
    smooth_value = linear_value = 0.2 + 0.4 * float(vertex);
    gl_Position = vec4(positions[vertex] / 32.0 - 1.0, 0, 1) * weights[vertex];
    gl_PointSize = 4.0 + 4.0 * float(vertex);
#if SHAPE == 1
    gl_PointSize = 2.0;
    gl_ClipDistance[0] = vertex == 0u ? -1.0 : 1.0;
#elif SHAPE == 2
    gl_PointSize = 2.0;
    gl_Position.z = gl_Position.w * (vertex == 0u ? -1.0 : 0.5);
#elif SHAPE == 3
    gl_CullDistance[0] = vertex == 0u ? -1.0 : 1.0;
#elif SHAPE == 4
    gl_CullDistance[0] = -1.0;
#elif SHAPE == 5
    gl_PointSize = 2.0;
    gl_Position.y = -0.5 * gl_Position.w;
#elif SHAPE == 6
    gl_Position.z = gl_Position.w * (0.25 + 0.125 * float(vertex));
#endif
#if CAPTURE
    tag = uvec4(vertex, 7, 11, 13);
#endif
}
