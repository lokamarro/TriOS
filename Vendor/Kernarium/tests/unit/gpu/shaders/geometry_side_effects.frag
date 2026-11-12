// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) flat in uvec4 tag;
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(tag.y == 0u ? 1.0 : 0.0, tag.y == 1u ? 1.0 : 0.0,
                 float(tag.z), 1.0);
}
