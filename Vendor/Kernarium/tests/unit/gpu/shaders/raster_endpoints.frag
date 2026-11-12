// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(early_fragment_tests) in;
layout(set = 0, binding = 0, std430) buffer Data { uint words[]; } data;
layout(location = 0) out vec4 color;
void main()
{
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    atomicAdd(data.words[32u + pixel.y * 64u + pixel.x], 1u);
    color = vec4(1);
}
