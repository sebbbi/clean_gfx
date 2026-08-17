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

template<typename T>
concept has_push_root = requires(T& commands) { commands.push_root(0u); };

template<typename T>
concept has_push_data = requires(T& commands) {
    commands.push_data(std::span<const std::byte>{});
};

template<typename T>
concept has_rootless_draw = requires(T& commands) { commands.draw(3u); };

template<typename T>
concept has_rootless_dispatch = requires(T& commands) { commands.dispatch(1u); };

struct ApiRoot
{
    std::uint64_t vertices;
    std::uint32_t texture_index;
    float scale;
};

static_assert(!std::is_copy_constructible_v<gfx::Device>);
static_assert(std::is_nothrow_move_constructible_v<gfx::Device>);
static_assert(!std::is_copy_constructible_v<gfx::Texture>);
static_assert(std::is_nothrow_move_constructible_v<gfx::Texture>);
static_assert(!std::is_copy_constructible_v<gfx::Pipeline>);
static_assert(std::is_nothrow_move_constructible_v<gfx::Pipeline>);
static_assert(!std::is_copy_constructible_v<gfx::CommandList>);
static_assert(std::is_nothrow_move_constructible_v<gfx::CommandList>);
static_assert(std::is_enum_v<gfx::Error>);
static_assert(std::is_same_v<std::underlying_type_t<gfx::Error>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(gfx::Error::none) == 0);
static_assert(std::is_enum_v<gfx::MemoryType>);
static_assert(std::is_same_v<std::underlying_type_t<gfx::MemoryType>, std::uint8_t>);
static_assert(gfx::MemoryType::default_ != gfx::MemoryType::gpu_only);
static_assert(gfx::MemoryType::gpu_only != gfx::MemoryType::readback);
static_assert(std::is_enum_v<gfx::TextureDescriptorType>);
static_assert(std::is_same_v<
              std::underlying_type_t<gfx::TextureDescriptorType>,
              std::uint8_t>);
static_assert(std::is_aggregate_v<gfx::DeviceDesc>);
static_assert(std::is_standard_layout_v<gfx::DeviceDesc>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceDesc>);
static_assert(sizeof(gfx::DeviceDesc) == sizeof(const char*));
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
static_assert(std::is_aggregate_v<gfx::TextureViewDesc>);
static_assert(std::is_standard_layout_v<gfx::TextureViewDesc>);
static_assert(std::is_trivially_copyable_v<gfx::TextureViewDesc>);
static_assert(sizeof(gfx::TextureViewDesc) == 8);
static_assert(offsetof(gfx::TextureViewDesc, base_mip) == 0);
static_assert(offsetof(gfx::TextureViewDesc, mip_count) == 4);
constexpr gfx::TextureViewDesc default_texture_view{};
static_assert(default_texture_view.base_mip == 0);
static_assert(default_texture_view.mip_count == 0);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().gpu_malloc(16)),
              gfx::GpuAllocation>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>()
                           .gpu_malloc<std::uint32_t>(4)),
              gfx::GpuAllocation>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().gpu_malloc(
                  16, gfx::MemoryType::gpu_only)),
              gfx::GpuAllocation>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>()
                           .gpu_malloc_resource_heap(16)),
              gfx::GpuAllocation>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>()
                           .gpu_malloc_sampler_heap(16)),
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
              decltype(std::declval<const gfx::Device&>().write_texture_descriptor(
                  std::declval<void*>(),
                  std::declval<const gfx::Texture&>(),
                  gfx::TextureDescriptorType::sampled,
                  std::declval<const gfx::TextureViewDesc&>())),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().write_sampler_descriptor(
                  std::declval<void*>(),
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
              decltype(std::declval<gfx::CommandList&>().set_resource_heap(
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().set_sampler_heap(
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(!has_push_root<gfx::CommandList>);
static_assert(!has_push_data<gfx::CommandList>);
static_assert(!has_rootless_draw<gfx::CommandList>);
static_assert(!has_rootless_dispatch<gfx::CommandList>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw(
                  std::declval<const ApiRoot*>(), 3u)),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw(nullptr, 3u)),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw_indexed(
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint16,
                  3u)),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw_indexed(
                  nullptr,
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint16,
                  3u)),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw_indirect(
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw_indirect(
                  nullptr,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw_indexed_indirect(
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint32,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().draw_indexed_indirect(
                  nullptr,
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint32,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().dispatch(
                  std::declval<const ApiRoot*>(), 1u)),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().dispatch(nullptr, 1u)),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().dispatch_indirect(
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().dispatch_indirect(
                  nullptr,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<gfx::CommandList&>().finish()),
              gfx::Error>);
static_assert(std::is_same_v<
              decltype(std::declval<const gfx::Device&>().submit(
                  std::declval<gfx::CommandList&&>())),
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
