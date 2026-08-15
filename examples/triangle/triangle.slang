#include "triangle_shared.h"

[[vk::push_constant]]
ConstantBuffer<RootArguments> root;

#if defined(CLEAN_GFX_DESCRIPTOR_MAPPING_FALLBACK)
[[vk::binding(0, 0)]] Texture2D<float4> mapped_textures[];
[[vk::binding(1, 0)]] SamplerState mapped_samplers[];
[[vk::binding(2, 0)]] RWTexture2D<float4> mapped_storage_textures[];
#endif

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    Vertex vertex = root.vertices[vertex_id];
    VertexOutput output;
    output.position = float4(vertex.position, 0.0f, 1.0f);
    output.uv = vertex.uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
#if defined(CLEAN_GFX_NATIVE_DESCRIPTOR_HEAP)
    Texture2D<float4> texture = ResourceDescriptorHeap[root.texture_index];
    SamplerState sampler = SamplerDescriptorHeap[root.sampler_index];
#else
    Texture2D<float4> texture = mapped_textures[root.texture_index];
    SamplerState sampler = mapped_samplers[root.sampler_index];
#endif
    const float4 tint = float4(float2(root.tint_rg), float2(root.tint_ba));
    return texture.Sample(sampler, input.uv) * tint * root.exposure;
}

[shader("compute")]
[numthreads(2, 2, 1)]
void computeMain(uint3 dispatch_id : SV_DispatchThreadID)
{
#if defined(CLEAN_GFX_NATIVE_DESCRIPTOR_HEAP)
    RWTexture2D<float4> output = ResourceDescriptorHeap[root.texture_index];
#else
    RWTexture2D<float4> output = mapped_storage_textures[root.texture_index];
#endif
    const bool alternate = ((dispatch_id.x ^ dispatch_id.y) & 1u) != 0;
    output[dispatch_id.xy] = alternate ? float4(0.25f, 1.0f, 0.25f, 1.0f)
                                       : float4(1.0f, 0.25f, 0.25f, 1.0f);
}
