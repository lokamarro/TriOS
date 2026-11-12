// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#ifndef CONSTANT_COLOR
layout(location = 0) in vec3 coords;
layout(location = 1) flat in uvec3 ids;
#endif
layout(location = 0) out vec4 color;
void main()
{
#ifdef CONSTANT_COLOR
    color = vec4(0.25, 0.5, 0.75, 1.0);
#else
    color = vec4(coords.xy, float(ids.z & 255u) / 255.0, 1.0);
#endif
}
