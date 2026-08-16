#include "triangle_shared.h"

[[vk::push_constant]]
ConstantBuffer<RootArguments> root;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    Vertex vertex = root.vertices[vertex_id];
    VertexOutput output;
    output.position = float4(vertex.position, 1.0f);
    output.color = vertex.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0f);
}
