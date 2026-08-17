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
#include <cstdint>
#include <type_traits>
#include <utility>

template<typename T>
concept complete_type = requires { sizeof(T); };

template<typename T>
concept has_legacy_device_methods = requires(T* device) {
    device->caps();
    device->gpu_malloc(16);
    device->wait_idle();
};

template<typename T>
concept has_legacy_command_methods = requires(T* commands) {
    commands->draw(nullptr, 3u);
    commands->dispatch(nullptr, 1u);
    commands->finish();
};

template<typename T>
concept has_rootless_draw = requires(T* commands) { gfx::draw(commands, 3u); };

template<typename T>
concept has_rootless_dispatch = requires(T* commands) { gfx::dispatch(commands, 1u); };

struct ApiRoot
{
    std::uint64_t vertices;
    std::uint32_t texture_index;
    float scale;
};

static_assert(!complete_type<gfx::Device>);
static_assert(!complete_type<gfx::Texture>);
static_assert(!complete_type<gfx::Pipeline>);
static_assert(!complete_type<gfx::CommandList>);
static_assert(!has_legacy_device_methods<gfx::Device>);
static_assert(!has_legacy_command_methods<gfx::CommandList>);

static_assert(std::is_enum_v<gfx::Error>);
static_assert(std::is_same_v<std::underlying_type_t<gfx::Error>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(gfx::Error::none) == 0);
static_assert(std::is_enum_v<gfx::MemoryType>);
static_assert(std::is_same_v<std::underlying_type_t<gfx::MemoryType>, std::uint8_t>);
static_assert(gfx::MemoryType::default_ != gfx::MemoryType::gpu_only);
static_assert(gfx::MemoryType::gpu_only != gfx::MemoryType::readback);
static_assert(std::is_same_v<
              std::underlying_type_t<gfx::TextureDescriptorType>,
              std::uint8_t>);

static_assert(std::is_aggregate_v<gfx::DeviceDesc>);
static_assert(std::is_standard_layout_v<gfx::DeviceDesc>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceDesc>);
static_assert(sizeof(gfx::DeviceDesc) == sizeof(const char*));

static_assert(std::is_aggregate_v<gfx::DeviceInit>);
static_assert(std::is_trivial_v<gfx::DeviceInit>);
static_assert(std::is_standard_layout_v<gfx::DeviceInit>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceInit>);
static_assert(offsetof(gfx::DeviceInit, device) == 0);
static_assert(offsetof(gfx::DeviceInit, error) == sizeof(gfx::Device*));
constexpr gfx::DeviceInit empty_device{};
static_assert(empty_device.device == nullptr);
static_assert(empty_device.error == gfx::Error::none);

static_assert(std::is_aggregate_v<gfx::DeviceCaps>);
static_assert(std::is_trivial_v<gfx::DeviceCaps>);
static_assert(std::is_standard_layout_v<gfx::DeviceCaps>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceCaps>);
static_assert(std::is_same_v<decltype(gfx::DeviceCaps::device_name), const char*>);

static_assert(std::is_aggregate_v<gfx::GpuRange>);
static_assert(std::is_trivial_v<gfx::GpuRange>);
static_assert(std::is_standard_layout_v<gfx::GpuRange>);
static_assert(std::is_trivially_copyable_v<gfx::GpuRange>);
static_assert(sizeof(gfx::GpuRange) == 16);
static_assert(offsetof(gfx::GpuRange, gpu) == 0);
static_assert(offsetof(gfx::GpuRange, size) == 8);

using ByteAllocation = gfx::GpuAllocation<>;
using TypedAllocation = gfx::GpuAllocation<std::uint32_t>;
static_assert(std::is_same_v<ByteAllocation, gfx::GpuAllocation<std::byte>>);
static_assert(std::is_aggregate_v<ByteAllocation>);
static_assert(std::is_trivial_v<ByteAllocation>);
static_assert(std::is_standard_layout_v<ByteAllocation>);
static_assert(std::is_trivially_copyable_v<ByteAllocation>);
static_assert(std::is_aggregate_v<TypedAllocation>);
static_assert(std::is_trivial_v<TypedAllocation>);
static_assert(std::is_standard_layout_v<TypedAllocation>);
static_assert(std::is_trivially_copyable_v<TypedAllocation>);
static_assert(sizeof(ByteAllocation) == 24);
static_assert(sizeof(TypedAllocation) == 24);
static_assert(offsetof(ByteAllocation, cpu) == 0);
static_assert(offsetof(ByteAllocation, gpu) == 8);
static_assert(offsetof(ByteAllocation, size) == 16);
static_assert(std::is_same_v<decltype(TypedAllocation::cpu), std::uint32_t*>);
static_assert(std::is_same_v<decltype(TypedAllocation::gpu), std::uint32_t*>);
constexpr ByteAllocation null_allocation{};
static_assert(null_allocation.cpu == nullptr);
static_assert(null_allocation.gpu == nullptr);
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
              decltype(gfx::create_device(
                  std::declval<const gfx::DeviceDesc&>())),
              gfx::DeviceInit>);
static_assert(std::is_same_v<
              decltype(gfx::destroy_device(std::declval<gfx::Device*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::get_device_caps(std::declval<const gfx::Device*>())),
              gfx::DeviceCaps>);

static_assert(std::is_same_v<
              decltype(gfx::gpu_malloc(std::declval<gfx::Device*>(), 16)),
              ByteAllocation>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_malloc(
                  std::declval<gfx::Device*>(), 16,
                  gfx::MemoryType::gpu_only, 256)),
              ByteAllocation>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_malloc<std::uint32_t>(
                  std::declval<gfx::Device*>(), 4)),
              TypedAllocation>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_malloc_resource_heap(
                  std::declval<gfx::Device*>(), 16)),
              ByteAllocation>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_malloc_sampler_heap(
                  std::declval<gfx::Device*>(), 16)),
              ByteAllocation>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_free(
                  std::declval<gfx::Device*>(),
                  std::declval<ByteAllocation>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_free(
                  std::declval<gfx::Device*>(),
                  std::declval<TypedAllocation>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::gpu_range(std::declval<TypedAllocation>())),
              gfx::GpuRange>);

static_assert(std::is_same_v<
              decltype(gfx::create_texture(
                  std::declval<gfx::Device*>(),
                  std::declval<const gfx::TextureDesc&>())),
              gfx::Texture*>);
static_assert(std::is_same_v<
              decltype(gfx::destroy_texture(std::declval<gfx::Texture*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::texture_width(std::declval<const gfx::Texture*>())),
              std::uint32_t>);
static_assert(std::is_same_v<
              decltype(gfx::texture_height(std::declval<const gfx::Texture*>())),
              std::uint32_t>);
static_assert(std::is_same_v<
              decltype(gfx::write_texture_descriptor(
                  std::declval<gfx::Device*>(),
                  std::declval<void*>(),
                  std::declval<const gfx::Texture*>(),
                  gfx::TextureDescriptorType::sampled,
                  std::declval<const gfx::TextureViewDesc&>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::write_sampler_descriptor(
                  std::declval<gfx::Device*>(),
                  std::declval<void*>(),
                  std::declval<const gfx::SamplerDesc&>())),
              void>);

static_assert(std::is_same_v<
              decltype(gfx::create_graphics_pipeline(
                  std::declval<gfx::Device*>(),
                  std::declval<const gfx::GraphicsPipelineDesc&>())),
              gfx::Pipeline*>);
static_assert(std::is_same_v<
              decltype(gfx::create_compute_pipeline(
                  std::declval<gfx::Device*>(),
                  std::declval<const gfx::ComputePipelineDesc&>())),
              gfx::Pipeline*>);
static_assert(std::is_same_v<
              decltype(gfx::destroy_pipeline(std::declval<gfx::Pipeline*>())),
              void>);

static_assert(std::is_same_v<
              decltype(gfx::begin_commands(std::declval<gfx::Device*>())),
              gfx::CommandList*>);
static_assert(std::is_same_v<
              decltype(gfx::destroy_command_list(
                  std::declval<gfx::CommandList*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::submit(
                  std::declval<gfx::Device*>(),
                  std::declval<gfx::CommandList*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::submit_and_wait(
                  std::declval<gfx::Device*>(),
                  std::declval<gfx::CommandList*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::wait_idle(std::declval<gfx::Device*>())),
              void>);

static_assert(std::is_same_v<
              decltype(gfx::bind_pipeline(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const gfx::Pipeline*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::set_resource_heap(
                  std::declval<gfx::CommandList*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::set_sampler_heap(
                  std::declval<gfx::CommandList*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::begin_rendering(
                  std::declval<gfx::CommandList*>(),
                  std::declval<gfx::Texture*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::end_rendering(std::declval<gfx::CommandList*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::copy_memory(
                  std::declval<gfx::CommandList*>(),
                  std::declval<gfx::GpuRange>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::copy_memory_to_texture(
                  std::declval<gfx::CommandList*>(),
                  std::declval<gfx::GpuRange>(),
                  std::declval<gfx::Texture*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::copy_texture_to_memory(
                  std::declval<gfx::CommandList*>(),
                  std::declval<gfx::Texture*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::barrier(
                  std::declval<gfx::CommandList*>(),
                  gfx::Stage::transfer,
                  gfx::Access::transfer_write,
                  gfx::Stage::fragment,
                  gfx::Access::shader_read)),
              void>);

static_assert(!has_rootless_draw<gfx::CommandList>);
static_assert(!has_rootless_dispatch<gfx::CommandList>);
static_assert(std::is_same_v<
              decltype(gfx::draw(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const ApiRoot*>(), 3u)),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw(
                  std::declval<gfx::CommandList*>(), nullptr, 3u)),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw_indexed(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint16, 3u)),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw_indexed(
                  std::declval<gfx::CommandList*>(), nullptr,
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint16, 3u)),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw_indirect(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw_indirect(
                  std::declval<gfx::CommandList*>(), nullptr,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw_indexed_indirect(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint32,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::draw_indexed_indirect(
                  std::declval<gfx::CommandList*>(), nullptr,
                  std::declval<gfx::GpuRange>(),
                  gfx::IndexType::uint32,
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::dispatch(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const ApiRoot*>(), 1u)),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::dispatch(
                  std::declval<gfx::CommandList*>(), nullptr, 1u)),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::dispatch_indirect(
                  std::declval<gfx::CommandList*>(),
                  std::declval<const ApiRoot*>(),
                  std::declval<gfx::GpuRange>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::dispatch_indirect(
                  std::declval<gfx::CommandList*>(), nullptr,
                  std::declval<gfx::GpuRange>())),
              void>);

[[maybe_unused]] void instantiate_template_bodies(
    gfx::Device* device,
    gfx::CommandList* commands,
    const ApiRoot* root,
    TypedAllocation allocation)
{
    const auto typed = gfx::gpu_malloc<std::uint32_t>(
        device, 4, gfx::MemoryType::default_);
    gfx::gpu_free(device, allocation);
    const auto range = gfx::gpu_range(typed);
    gfx::draw(commands, root, 3u);
    gfx::draw_indexed(
        commands, root, range, gfx::IndexType::uint32, 3u);
    gfx::draw_indirect(commands, root, range);
    gfx::draw_indexed_indirect(
        commands, root, range, gfx::IndexType::uint32, range);
    gfx::dispatch(commands, root, 1u);
    gfx::dispatch_indirect(commands, root, range);
}

int main()
{
    return 0;
}
