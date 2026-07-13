#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full-screen quad as a triangle-strip, no vertex buffer needed -- the four
// corners and their UVs are derived directly from vertex_id.
vertex VertexOut display_rfb_vertex(uint vertexID [[vertex_id]]) {
    float2 positions[4] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
    };
    float2 uvs[4] = {
        float2(0.0, 1.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 0.0),
    };
    VertexOut out;
    out.position = float4(positions[vertexID], 0.0, 1.0);
    out.uv = uvs[vertexID];
    return out;
}

fragment float4 display_rfb_fragment(VertexOut in [[stage_in]],
                                      texture2d<float> framebufferTexture [[texture(0)]]) {
    constexpr sampler texSampler(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    float4 color = framebufferTexture.sample(texSampler, in.uv);
    // The alpha byte is RFB padding, not real alpha -- force opaque.
    return float4(color.rgb, 1.0);
}
