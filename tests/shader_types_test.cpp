#include <clean_gfx/shader_types.h>

#include <cstddef>
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

template<typename Type, typename Scalar, std::size_t ComponentCount>
inline constexpr bool is_scalar_layout_vector =
    std::is_standard_layout_v<Type> &&
    std::is_trivial_v<Type> &&
    std::is_trivially_copyable_v<Type> &&
    std::is_trivially_default_constructible_v<Type> &&
    std::is_trivially_copy_constructible_v<Type> &&
    std::is_trivially_move_constructible_v<Type> &&
    std::is_trivially_copy_assignable_v<Type> &&
    std::is_trivially_move_assignable_v<Type> &&
    std::is_trivially_destructible_v<Type> &&
    std::is_aggregate_v<Type> &&
    sizeof(Type) == sizeof(Scalar) * ComponentCount &&
    alignof(Type) == alignof(Scalar);

static_assert(is_scalar_layout_vector<float16_t, uint16, 1>);
static_assert(is_scalar_layout_vector<float2, float, 2>);
static_assert(is_scalar_layout_vector<float3, float, 3>);
static_assert(is_scalar_layout_vector<float4, float, 4>);
static_assert(is_scalar_layout_vector<int2, int32, 2>);
static_assert(is_scalar_layout_vector<int3, int32, 3>);
static_assert(is_scalar_layout_vector<int4, int32, 4>);
static_assert(is_scalar_layout_vector<uint2, uint32, 2>);
static_assert(is_scalar_layout_vector<uint3, uint32, 3>);
static_assert(is_scalar_layout_vector<uint4, uint32, 4>);
static_assert(is_scalar_layout_vector<float16_t2, float16_t, 2>);
static_assert(is_scalar_layout_vector<float16_t3, float16_t, 3>);
static_assert(is_scalar_layout_vector<float16_t4, float16_t, 4>);
static_assert(is_scalar_layout_vector<int16_t2, int16, 2>);
static_assert(is_scalar_layout_vector<int16_t3, int16, 3>);
static_assert(is_scalar_layout_vector<int16_t4, int16, 4>);
static_assert(is_scalar_layout_vector<uint16_t2, uint16, 2>);
static_assert(is_scalar_layout_vector<uint16_t3, uint16, 3>);
static_assert(is_scalar_layout_vector<uint16_t4, uint16, 4>);

#define CLEAN_GFX_TEST_VEC2(type_, scalar_)                                                   \
    static_assert(std::is_same_v<decltype(type_::x), scalar_>);                               \
    static_assert(std::is_same_v<decltype(type_::y), scalar_>);                               \
    static_assert(offsetof(type_, x) == 0);                                                   \
    static_assert(offsetof(type_, y) == sizeof(scalar_))

#define CLEAN_GFX_TEST_VEC3(type_, scalar_)                                                   \
    CLEAN_GFX_TEST_VEC2(type_, scalar_);                                                      \
    static_assert(std::is_same_v<decltype(type_::z), scalar_>);                               \
    static_assert(offsetof(type_, z) == sizeof(scalar_) * 2)

#define CLEAN_GFX_TEST_VEC4(type_, scalar_)                                                   \
    CLEAN_GFX_TEST_VEC3(type_, scalar_);                                                      \
    static_assert(std::is_same_v<decltype(type_::w), scalar_>);                               \
    static_assert(offsetof(type_, w) == sizeof(scalar_) * 3)

CLEAN_GFX_TEST_VEC2(float2, float);
CLEAN_GFX_TEST_VEC3(float3, float);
CLEAN_GFX_TEST_VEC4(float4, float);
CLEAN_GFX_TEST_VEC2(int2, int32);
CLEAN_GFX_TEST_VEC3(int3, int32);
CLEAN_GFX_TEST_VEC4(int4, int32);
CLEAN_GFX_TEST_VEC2(uint2, uint32);
CLEAN_GFX_TEST_VEC3(uint3, uint32);
CLEAN_GFX_TEST_VEC4(uint4, uint32);
CLEAN_GFX_TEST_VEC2(float16_t2, float16_t);
CLEAN_GFX_TEST_VEC3(float16_t3, float16_t);
CLEAN_GFX_TEST_VEC4(float16_t4, float16_t);
CLEAN_GFX_TEST_VEC2(int16_t2, int16);
CLEAN_GFX_TEST_VEC3(int16_t3, int16);
CLEAN_GFX_TEST_VEC4(int16_t4, int16);
CLEAN_GFX_TEST_VEC2(uint16_t2, uint16);
CLEAN_GFX_TEST_VEC3(uint16_t3, uint16);
CLEAN_GFX_TEST_VEC4(uint16_t4, uint16);

#undef CLEAN_GFX_TEST_VEC4
#undef CLEAN_GFX_TEST_VEC3
#undef CLEAN_GFX_TEST_VEC2

static_assert(std::is_same_v<decltype(float16_t::bits), uint16>);
static_assert(offsetof(float16_t, bits) == 0);

constexpr float16_t half_one{0x3c00u};
constexpr float16_t half_negative_two{0xc000u};
constexpr float16_t2 half_pair{half_one, half_negative_two};
constexpr int16_t3 narrow_ints{-1, 2, -3};
constexpr uint16_t4 narrow_uints{1u, 2u, 3u, 4u};
constexpr float3 floats{1.0f, 2.0f, 3.0f};
constexpr int4 ints{-1, 2, -3, 4};
constexpr uint4 uints{1u, 2u, 3u, 4u};

static_assert(half_one.bits == 0x3c00u);
static_assert(half_negative_two.bits == 0xc000u);
static_assert(half_pair.x.bits == 0x3c00u && half_pair.y.bits == 0xc000u);
static_assert(narrow_ints.x == -1 && narrow_ints.z == -3);
static_assert(narrow_uints.x == 1u && narrow_uints.w == 4u);
static_assert(floats.x == 1.0f && floats.z == 3.0f);
static_assert(ints.x == -1 && ints.z == -3);
static_assert(uints.x == 1u && uints.w == 4u);

int main()
{
    return 0;
}
