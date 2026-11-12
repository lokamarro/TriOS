// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_EXT_multiview : require
#ifdef WRITE_LAYER
#extension GL_ARB_shader_viewport_layer_array : require
#endif

layout(set = 0, binding = 0, std430) buffer Counters {
    uint invocations[16];
};
layout(location = 0) flat out vec4 color;

void main()
{
    uint view = uint(gl_ViewIndex);
    uint instance = uint(gl_InstanceIndex) - 3u;
    uint vertex = uint(gl_VertexIndex) - 5u;
    atomicAdd(invocations[view * 2u + instance], 1u);
    float x = 8.5 + float(vertex) * (8.0 + float(view) * 2.0);
    float y = 8.5 + float(view) * 10.0 + float(instance) * 4.0;
    gl_Position = vec4(x / 32.0 - 1.0, y / 32.0 - 1.0, 0.5, 1.0);
#ifdef WRITE_LAYER
    gl_Layer = 3;
#endif
    color = vec4(float(view + 1u) / 8.0,
                 float(vertex + 1u) / 4.0,
                 float(instance + 1u) / 4.0, 1.0);
}
