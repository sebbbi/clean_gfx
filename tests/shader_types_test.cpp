#include <clean_gfx/shader_types.h>

#include <cstdint>
#include <type_traits>

static_assert(std::is_same_v<uint8, std::uint8_t>);
static_assert(std::is_same_v<int8, std::int8_t>);
static_assert(std::is_same_v<uint16, std::uint16_t>);
static_assert(std::is_same_v<int16, std::int16_t>);
static_assert(std::is_same_v<uint32, std::uint32_t>);
static_assert(std::is_same_v<int32, std::int32_t>);
static_assert(std::is_same_v<uint64, std::uint64_t>);
static_assert(std::is_same_v<int64, std::int64_t>);

constexpr float16_t half_one{.bits = 0x3c00u};
constexpr float3 floats{.x = 1.0f, .y = 2.0f, .z = 3.0f};
constexpr int16_t3 narrow_ints{.x = -1, .y = 2, .z = -3};
constexpr uint4 uints{.x = 1u, .y = 2u, .z = 3u, .w = 4u};
constexpr float3x4 matrix{
    .rows = {
        {.x = 1.0f, .y = 2.0f, .z = 3.0f, .w = 4.0f},
        {.x = 5.0f, .y = 6.0f, .z = 7.0f, .w = 8.0f},
        {.x = 9.0f, .y = 10.0f, .z = 11.0f, .w = 12.0f},
    },
};

static_assert(sizeof(float3x4) == sizeof(float4) * 3);
static_assert(std::is_same_v<decltype(float3x4::rows), float4[3]>);
static_assert(half_one.bits == 0x3c00u);
static_assert(floats.x == 1.0f && floats.z == 3.0f);
static_assert(narrow_ints.x == -1 && narrow_ints.z == -3);
static_assert(uints.x == 1u && uints.w == 4u);
static_assert(matrix.rows[2].w == 12.0f);

int main()
{
    return 0;
}
