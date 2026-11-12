// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(vertices = 3) out;
layout(location = 1) flat in uint vertex_tag[];
layout(location = 1) flat out uint control_tag[];
layout(location = 2) in float vertex_smooth[];
layout(location = 3) noperspective in float vertex_linear[];
layout(location = 2) out float control_smooth[];
layout(location = 3) noperspective out float control_linear[];
void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_out[gl_InvocationID].gl_PointSize = gl_in[gl_InvocationID].gl_PointSize;
    control_tag[gl_InvocationID] = vertex_tag[gl_InvocationID];
    control_smooth[gl_InvocationID] = vertex_smooth[gl_InvocationID];
    control_linear[gl_InvocationID] = vertex_linear[gl_InvocationID];
#if SHAPE == 1
    gl_out[gl_InvocationID].gl_ClipDistance[0] = gl_in[gl_InvocationID].gl_ClipDistance[0];
#elif SHAPE == 3 || SHAPE == 4
    gl_out[gl_InvocationID].gl_CullDistance[0] = gl_in[gl_InvocationID].gl_CullDistance[0];
#endif
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = 1;
        gl_TessLevelOuter[1] = 1;
        gl_TessLevelOuter[2] = 1;
        gl_TessLevelInner[0] = 1;
    }
}
