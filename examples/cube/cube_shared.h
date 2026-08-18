#pragma once

#include <clean_gfx/shader_types.h>

struct CubeVertex
{
    float4 position;
    float2 uv;
};

enum class CubeSampler : uint32
{
    wrap_linear,
    wrap_point,
    clamp_linear,
    clamp_point,
    count,
};

struct CubeRootArguments
{
    CubeVertex* vertices;
    float3x4 transform;
    float2 depth_transform;
    uint32 texture_index;
};
