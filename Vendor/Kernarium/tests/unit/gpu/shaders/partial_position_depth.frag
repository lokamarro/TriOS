// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) out vec4 output_color;
void main()
{
    output_color = vec4(1.0, 0.0, 0.0, 1.0);
    gl_FragDepth = 0.5;
}
