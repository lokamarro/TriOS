// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) flat in vec4 color;
layout(location = 0) out vec4 result;
void main()
{
    result = color;
}
