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
    uint32 texture_index;
    uint32 padding;
    float4x4 mvp;
};
