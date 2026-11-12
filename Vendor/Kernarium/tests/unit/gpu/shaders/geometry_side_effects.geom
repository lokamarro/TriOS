// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(points) in;
layout(line_strip, max_vertices = 4) out;
layout(set = 0, binding = 0, std430) buffer Counters { uint counters[]; };
#if CAPTURE
layout(location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0)
flat out uvec4 tag;
#else
layout(location = 0) flat out uvec4 tag;
#endif

void emit_vertex(float x, float y, uint ordinal, uint strip, uint previous)
{
    gl_Position = vec4(vec2(x, y) / 32.0 - 1.0, 0.0, 1.0);
    tag = uvec4(ordinal, strip, previous, 123u + uint(gl_PrimitiveIDIn));
    EmitVertex();
}

void main()
{
    atomicAdd(counters[0], 1u);
#if USE_RESULT
    uint previous = atomicAdd(counters[2 + gl_PrimitiveIDIn], 1u);
    if (previous != 0u)
        return;
#else
    atomicAdd(counters[2 + gl_PrimitiveIDIn], 1u);
    uint previous = 0u;
#endif
#if SHAPE == 0
    emit_vertex(8.5, 16.5, 0u, 0u, previous);
    emit_vertex(40.5, 16.5, 1u, 0u, previous);
    EndPrimitive();
    emit_vertex(8.5, 40.5, 2u, 1u, previous);
    emit_vertex(40.5, 40.5, 3u, 1u, previous);
    EndPrimitive();
#elif SHAPE == 1
    emit_vertex(8.5, 16.5, 0u, 0u, previous);
    emit_vertex(24.5, 16.5, 1u, 0u, previous);
    emit_vertex(40.5, 16.5, 2u, 0u, previous);
    EndPrimitive();
#endif
    atomicAdd(counters[1], 7u);
}
