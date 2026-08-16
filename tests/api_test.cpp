#if defined(_CPPUNWIND) || defined(__EXCEPTIONS) || defined(__cpp_exceptions)
#error clean_gfx tests must be compiled with C++ exceptions disabled
#endif

#if defined(__clang__)
#if __has_feature(cxx_exceptions)
#error clean_gfx tests must be compiled with C++ exceptions disabled
#endif
#endif

#if defined(_HAS_EXCEPTIONS) && _HAS_EXCEPTIONS
#error clean_gfx tests must use the no-exceptions standard-library mode
#endif

#include <clean_gfx/clean_gfx.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<gfx::Device>);
static_assert(std::is_nothrow_move_constructible_v<gfx::Device>);
static_assert(std::is_enum_v<gfx::Error>);
static_assert(std::is_same_v<std::underlying_type_t<gfx::Error>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(gfx::Error::none) == 0);
static_assert(std::is_aggregate_v<gfx::GpuRange>);
static_assert(std::is_trivial_v<gfx::GpuRange>);
static_assert(std::is_standard_layout_v<gfx::GpuRange>);
static_assert(std::is_trivially_copyable_v<gfx::GpuRange>);
static_assert(sizeof(gfx::GpuRange) == 16);
static_assert(offsetof(gfx::GpuRange, gpu_ptr) == 0);
static_assert(offsetof(gfx::GpuRange, size) == 8);
static_assert(std::is_aggregate_v<gfx::GpuAllocation>);
static_assert(std::is_trivial_v<gfx::GpuAllocation>);
static_assert(std::is_standard_layout_v<gfx::GpuAllocation>);
static_assert(std::is_trivially_copyable_v<gfx::GpuAllocation>);
static_assert(sizeof(gfx::GpuAllocation) == 24);
static_assert(offsetof(gfx::GpuAllocation, cpu_ptr) == 0);
static_assert(offsetof(gfx::GpuAllocation, gpu_ptr) == 8);
static_assert(offsetof(gfx::GpuAllocation, size) == 16);
constexpr gfx::GpuAllocation null_allocation{};
static_assert(null_allocation.cpu_ptr == nullptr);
static_assert(null_allocation.gpu_ptr == nullptr);
static_assert(null_allocation.size == 0);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().gpu_malloc(16)),
              gfx::GpuAllocation>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>()
                           .gpu_malloc<std::uint32_t>(4)),
              gfx::GpuAllocation>);
static_assert(std::is_same_v<
              decltype(gfx::Device::create(
                  std::declval<gfx::Device&>(),
                  std::declval<const gfx::DeviceDesc&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().create_texture(
                  std::declval<gfx::Texture&>(),
                  std::declval<const gfx::TextureDesc&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().create_sampler(
                  std::declval<gfx::Sampler&>(),
                  std::declval<const gfx::SamplerDesc&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().create_graphics_pipeline(
                  std::declval<gfx::Pipeline&>(),
                  std::declval<const gfx::GraphicsPipelineDesc&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().create_compute_pipeline(
                  std::declval<gfx::Pipeline&>(),
                  std::declval<const gfx::ComputePipelineDesc&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().begin_commands(
                  std::declval<gfx::CommandList&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().finish()),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().submit_and_wait(
                  std::declval<gfx::CommandList&&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().wait_idle()),
              gfx::Error>);

int main()
{
    return 0;
}
