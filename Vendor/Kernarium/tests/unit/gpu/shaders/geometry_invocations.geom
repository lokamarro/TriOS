// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(points, invocations = INVOCATIONS) in;
// The generator changes only OutputPoints to OutputLineStrip. Vulkan permits
// multistream lines with transformFeedbackStreamsLinesTriangles, whereas GLSL
// frontends can retain the language's points-only multistream restriction.
layout(points, max_vertices = 4) out;
layout(set = 0, binding = 0, std430) buffer Counters { uint counters[]; };
layout(stream = 0, location = 1) out vec4 color;
layout(stream = 1, location = 0, xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0)
out uvec4 capture_tag;
void main()
{
    uint slot = 1u + uint(gl_PrimitiveIDIn) * INVOCATIONS + uint(gl_InvocationID);
    uint previous = atomicAdd(counters[slot], 1u);
    atomicAdd(counters[0], 1u);
    if (previous != 0u)
        return;
    for (uint vertex = 0u; vertex < 2u; ++vertex) {
        vec2 position = vec2(vertex == 0u ? 8.5 : 40.5,
                             8.5 + 12.0 * float(gl_InvocationID));
        gl_Position = vec4(position / 32.0 - 1.0, 0.0, 1.0);
        color = vec4(1.0, 0.0, 0.0, 1.0);
        EmitStreamVertex(0);
        capture_tag = uvec4(uint(gl_PrimitiveIDIn), uint(gl_InvocationID), vertex, 0x51u);
        EmitStreamVertex(1);
    }
#if EXPLICIT_END
    EndStreamPrimitive(0);
    EndStreamPrimitive(1);
#endif
}
