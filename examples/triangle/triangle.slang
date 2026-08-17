static const float3 positions[3] = {
    float3(0.5f, -0.5f, 0.5f),
    float3(0.5f, 0.5f, 0.5f),
    float3(-0.5f, 0.5f, 0.5f),
};

static const float3 colors[3] = {
    float3(1.0f, 0.0f, 0.0f),
    float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, 0.0f, 1.0f),
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    output.position = float4(positions[vertex_id], 1.0f);
    output.color = colors[vertex_id];
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0f);
}
