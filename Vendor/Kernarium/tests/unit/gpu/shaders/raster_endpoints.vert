// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(set = 0, binding = 0, std430) readonly buffer Data { uint words[]; } data;
void main()
{
    uint index = 4u * uint(gl_VertexIndex);
    vec4 position = uintBitsToFloat(uvec4(data.words[index], data.words[index + 1u],
                                        data.words[index + 2u], data.words[index + 3u]));
    gl_Position = vec4(position.xy / 32.0 - 1.0, position.z, 1.0) * position.w;
#if POINT_SIZE_STORAGE
    gl_PointSize = uintBitsToFloat(data.words[16u + uint(gl_VertexIndex)]);
#endif
}
