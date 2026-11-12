// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(vertices = 3) out;
layout(location = 0) flat in uint instance_id[];
layout(location = 0) flat out uint out_instance[];
layout(set = 0, binding = 0, std430) buffer Data { uint words[]; } data;
void main()
{
    atomicAdd(data.words[6], 1);
    out_instance[gl_InvocationID] = instance_id[gl_InvocationID];
    if (gl_InvocationID == 0) {
        atomicAdd(data.words[7], 1);
        const float level = float(data.words[0]);
        gl_TessLevelInner[0] = level;
        gl_TessLevelInner[1] = level;
        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelOuter[3] = level;
    }
}
