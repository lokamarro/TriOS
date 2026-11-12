// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location = 1) flat in uint control_tag[];
layout(location = 1) flat out uint flat_tag;
layout(location = 2) in float control_smooth[];
layout(location = 3) noperspective in float control_linear[];
layout(location = 2) out float smooth_value;
layout(location = 3) noperspective out float linear_value;
void main()
{
    flat_tag = control_tag[gl_TessCoord.x > 0.5 ? 0 : gl_TessCoord.y > 0.5 ? 1 : 2];
    smooth_value = dot(vec3(control_smooth[0], control_smooth[1], control_smooth[2]), gl_TessCoord);
    linear_value = dot(vec3(control_linear[0], control_linear[1], control_linear[2]), gl_TessCoord);
    gl_Position = gl_in[0].gl_Position * gl_TessCoord.x +
                  gl_in[1].gl_Position * gl_TessCoord.y +
                  gl_in[2].gl_Position * gl_TessCoord.z;
    gl_PointSize = gl_in[0].gl_PointSize * gl_TessCoord.x +
                   gl_in[1].gl_PointSize * gl_TessCoord.y +
                   gl_in[2].gl_PointSize * gl_TessCoord.z;
#if SHAPE == 1
    gl_ClipDistance[0] = gl_in[0].gl_ClipDistance[0] * gl_TessCoord.x +
                         gl_in[1].gl_ClipDistance[0] * gl_TessCoord.y +
                         gl_in[2].gl_ClipDistance[0] * gl_TessCoord.z;
#elif SHAPE == 3 || SHAPE == 4
    gl_CullDistance[0] = gl_in[0].gl_CullDistance[0] * gl_TessCoord.x +
                         gl_in[1].gl_CullDistance[0] * gl_TessCoord.y +
                         gl_in[2].gl_CullDistance[0] * gl_TessCoord.z;
#endif
}
