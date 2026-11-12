// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) out vec4 color;
layout(location = 1) flat in uint flat_tag;
#if ATTRIBUTES
layout(location = 2) in float smooth_value;
layout(location = 3) noperspective in float linear_value;
#endif
#if EARLY
layout(early_fragment_tests) in;
#endif
void main()
{
#if ATTRIBUTES
    color = vec4(gl_FrontFacing ? 1 : 0, smooth_value, linear_value, float(flat_tag + 1u) / 4.0);
#elif POINT_COORD
    color = vec4(gl_FrontFacing ? 1 : 0, gl_PointCoord, float(flat_tag + 1u) / 4.0);
#else
    color = vec4(gl_FrontFacing ? 1 : 0, 0, 0, float(flat_tag + 1u) / 4.0);
#endif
}
