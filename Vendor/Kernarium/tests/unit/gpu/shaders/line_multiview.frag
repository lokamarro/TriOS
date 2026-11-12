// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_EXT_multiview : require

layout(location = 0) flat in vec4 color;
layout(location = 0) out vec4 result;

void main()
{
    result = vec4(color.rgb, float(gl_ViewIndex + 1) / 8.0);
}
