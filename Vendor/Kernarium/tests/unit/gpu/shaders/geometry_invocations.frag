// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 output_color;
void main()
{
    output_color = color;
}
