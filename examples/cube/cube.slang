// Adapted from Vulkan-Tools' cube.vert and cube.frag and modified for Slang
// and clean_gfx. See NOTICE.md and LICENSE-Apache-2.0.txt.

#include "cube_shared.h"

[[vk::push_constant]]
ConstantBuffer<CubeRootArguments> root;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 fragment_position : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    CubeVertex vertex = root.vertices[vertex_id];
    float4 transformed = mul(root.mvp, vertex.position);

    VertexOutput output;
    output.position = transformed;
    output.uv = vertex.uv;
    output.fragment_position = transformed.xyz;
    return output;
}

float3 linear_to_srgb(float3 linear_color)
{
    float3 low = linear_color * 12.92f;
    float3 high = 1.055f * pow(linear_color, 1.0f / 2.4f) - 0.055f;
    return select(linear_color <= 0.0031308f, low, high);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
    SamplerState sampler = SamplerDescriptorHeap[uint32(CubeSampler::clamp_point)];

    float3 normal = normalize(cross(ddx(input.fragment_position),
                                    ddy(input.fragment_position)));
    float light = max(0.0f, dot(float3(0.424f, 0.566f, 0.707f), normal));
    float4 texel = texture.Sample(sampler, input.uv);
    return float4(linear_to_srgb(light * texel.rgb), light * texel.a);
}
