// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) flat out uint instance_id;
layout(set = 0, binding = 0, std430) buffer Data { uint words[]; } data;
void main()
{
    atomicAdd(data.words[5], 1);
    gl_Position = vec4(0, 0, 0, 1);
    instance_id = gl_InstanceIndex;
}
