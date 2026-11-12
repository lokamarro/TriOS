// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 1) flat in uint vertex_tag[];
layout(location = 1) flat out uint flat_tag;
layout(location = 2) in float vertex_smooth[];
layout(location = 3) noperspective in float vertex_linear[];
layout(location = 2) out float smooth_value;
layout(location = 3) noperspective out float linear_value;
void main()
{
    for (int vertex = 0; vertex < 3; ++vertex) {
        gl_Position = gl_in[vertex].gl_Position;
        gl_PointSize = gl_in[vertex].gl_PointSize;
        flat_tag = vertex_tag[vertex];
        smooth_value = vertex_smooth[vertex];
        linear_value = vertex_linear[vertex];
#if SHAPE == 1
        gl_ClipDistance[0] = gl_in[vertex].gl_ClipDistance[0];
#elif SHAPE == 3 || SHAPE == 4
        gl_CullDistance[0] = gl_in[vertex].gl_CullDistance[0];
#endif
        EmitVertex();
    }
    EndPrimitive();
}
