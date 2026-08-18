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
#include <initializer_list>
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

using ConstWordSpan = gfx::Span<const std::uint32_t>;
constexpr std::uint32_t shader_words[]{0x07230203u, 0x00010600u};
constexpr ConstWordSpan array_span{shader_words};
constexpr ConstWordSpan pointer_span{shader_words, 2};

constexpr bool has_spirv_header(const ConstWordSpan& words) noexcept
{
    return words.size == 2 && words.data[0] == 0x07230203u;
}

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

static_assert(std::is_standard_layout_v<ConstWordSpan>);
static_assert(std::is_trivially_copyable_v<ConstWordSpan>);
static_assert(sizeof(ConstWordSpan) == sizeof(const std::uint32_t*) +
                                           sizeof(std::size_t));
static_assert(std::is_same_v<decltype(ConstWordSpan::data),
                             const std::uint32_t*>);
static_assert(std::is_same_v<decltype(ConstWordSpan::size), std::size_t>);
static_assert(std::is_constructible_v<
              ConstWordSpan, std::initializer_list<std::uint32_t>>);
static_assert(!std::is_constructible_v<
              gfx::Span<std::uint32_t>,
              std::initializer_list<std::uint32_t>>);
static_assert(array_span.data == shader_words && array_span.size == 2);
static_assert(pointer_span.data == shader_words && pointer_span.size == 2);
static_assert(has_spirv_header({0x07230203u, 0x00010600u}));
constexpr ConstWordSpan empty_span{};
static_assert(empty_span.data == nullptr && empty_span.size == 0);

static_assert(std::is_aggregate_v<gfx::DeviceDesc>);
static_assert(std::is_standard_layout_v<gfx::DeviceDesc>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceDesc>);
static_assert(sizeof(gfx::DeviceDesc) == sizeof(const char*));
constexpr gfx::DeviceDesc default_device_desc{};
static_assert(default_device_desc.application_name != nullptr);

static_assert(std::is_aggregate_v<gfx::DeviceInit>);
static_assert(std::is_standard_layout_v<gfx::DeviceInit>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceInit>);
static_assert(offsetof(gfx::DeviceInit, device) == 0);
static_assert(offsetof(gfx::DeviceInit, error) == sizeof(gfx::Device*));
constexpr gfx::DeviceInit empty_device{};
static_assert(empty_device.device == nullptr);
static_assert(empty_device.error == gfx::Error::none);

static_assert(std::is_aggregate_v<gfx::DeviceCaps>);
static_assert(std::is_standard_layout_v<gfx::DeviceCaps>);
static_assert(std::is_trivially_copyable_v<gfx::DeviceCaps>);
static_assert(std::is_same_v<decltype(gfx::DeviceCaps::device_name), const char*>);
constexpr gfx::DeviceCaps empty_caps{};
static_assert(empty_caps.device_name == nullptr);
static_assert(empty_caps.api_version == 0);
static_assert(empty_caps.max_push_data_size == 0);
static_assert(empty_caps.image_descriptor_size == 0);
static_assert(empty_caps.sampler_descriptor_size == 0);

static_assert(std::is_aggregate_v<gfx::GpuRange>);
static_assert(std::is_standard_layout_v<gfx::GpuRange>);
static_assert(std::is_trivially_copyable_v<gfx::GpuRange>);
static_assert(sizeof(gfx::GpuRange) == 16);
static_assert(offsetof(gfx::GpuRange, gpu) == 0);
static_assert(offsetof(gfx::GpuRange, size) == 8);
constexpr gfx::GpuRange null_range{};
static_assert(null_range.gpu == nullptr && null_range.size == 0);

using ByteAllocation = gfx::GpuAllocation<>;
using TypedAllocation = gfx::GpuAllocation<std::uint32_t>;
static_assert(std::is_same_v<ByteAllocation, gfx::GpuAllocation<std::byte>>);
static_assert(std::is_aggregate_v<ByteAllocation>);
static_assert(std::is_standard_layout_v<ByteAllocation>);
static_assert(std::is_trivially_copyable_v<ByteAllocation>);
static_assert(std::is_aggregate_v<TypedAllocation>);
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

constexpr gfx::TextureDesc default_texture{};
static_assert(std::is_aggregate_v<gfx::TextureDesc>);
static_assert(std::is_standard_layout_v<gfx::TextureDesc>);
static_assert(std::is_trivially_copyable_v<gfx::TextureDesc>);
static_assert(default_texture.width == 1);
static_assert(default_texture.height == 1);
static_assert(default_texture.depth == 1);
static_assert(default_texture.mip_levels == 1);
static_assert(default_texture.format == gfx::Format::rgba8_unorm);
static_assert(default_texture.usage == gfx::TextureUsage::sampled);

constexpr gfx::SamplerDesc default_sampler{};
static_assert(std::is_aggregate_v<gfx::SamplerDesc>);
static_assert(std::is_standard_layout_v<gfx::SamplerDesc>);
static_assert(std::is_trivially_copyable_v<gfx::SamplerDesc>);
static_assert(default_sampler.min_filter == gfx::Filter::linear);
static_assert(default_sampler.mag_filter == gfx::Filter::linear);
static_assert(default_sampler.address_u == gfx::AddressMode::repeat);
static_assert(default_sampler.address_v == gfx::AddressMode::repeat);
static_assert(default_sampler.address_w == gfx::AddressMode::repeat);

constexpr gfx::GraphicsPipelineDesc default_graphics_pipeline{};
static_assert(std::is_aggregate_v<gfx::GraphicsPipelineDesc>);
static_assert(std::is_standard_layout_v<gfx::GraphicsPipelineDesc>);
static_assert(std::is_trivially_copyable_v<gfx::GraphicsPipelineDesc>);
static_assert(default_graphics_pipeline.vertex_spirv.data == nullptr);
static_assert(default_graphics_pipeline.vertex_spirv.size == 0);
static_assert(default_graphics_pipeline.fragment_spirv.data == nullptr);
static_assert(default_graphics_pipeline.fragment_spirv.size == 0);
static_assert(default_graphics_pipeline.color_format == gfx::Format::rgba8_unorm);
static_assert(default_graphics_pipeline.depth_format == gfx::Format::d32_float);
static_assert(!default_graphics_pipeline.depth_enabled);
static_assert(!default_graphics_pipeline.depth_write);
static_assert(default_graphics_pipeline.topology ==
              gfx::PrimitiveTopology::triangle_list);
static_assert(default_graphics_pipeline.cull == gfx::CullMode::none);

constexpr gfx::ComputePipelineDesc default_compute_pipeline{};
static_assert(std::is_aggregate_v<gfx::ComputePipelineDesc>);
static_assert(std::is_standard_layout_v<gfx::ComputePipelineDesc>);
static_assert(std::is_trivially_copyable_v<gfx::ComputePipelineDesc>);
static_assert(default_compute_pipeline.compute_spirv.data == nullptr);
static_assert(default_compute_pipeline.compute_spirv.size == 0);

static_assert(std::is_same_v<
              decltype(gfx::create_device(
                  std::declval<const gfx::DeviceDesc&>())),
              gfx::DeviceInit>);
static_assert(std::is_same_v<
              decltype(gfx::destroy_device(std::declval<gfx::Device*>())),
              void>);
static_assert(std::is_same_v<
              decltype(gfx::get_device_caps(std::declval<const gfx::Device*>())),
              const gfx::DeviceCaps&>);

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
using ByteFreeFunction =
    void (*)(gfx::Device*, const ByteAllocation&) noexcept;
static_assert(std::is_same_v<
              decltype(static_cast<ByteFreeFunction>(&gfx::gpu_free)),
              ByteFreeFunction>);

static_assert(std::is_same_v<
              decltype(gfx::create_texture(
                  std::declval<gfx::Device*>(),
                  std::declval<const gfx::TextureDesc&>())),
              gfx::Texture*>);
static_assert(std::is_same_v<
              decltype(gfx::destroy_texture(std::declval<gfx::Texture*>())),
              void>);
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

using SetHeapFunction =
    void (*)(gfx::CommandList*, const gfx::GpuRange&) noexcept;
static_assert(std::is_same_v<
              decltype(static_cast<SetHeapFunction>(&gfx::set_resource_heap)),
              SetHeapFunction>);
static_assert(std::is_same_v<
              decltype(static_cast<SetHeapFunction>(&gfx::set_sampler_heap)),
              SetHeapFunction>);

using BeginRenderingFunction = void (*)(gfx::CommandList*,
                                        gfx::Texture*,
                                        const float4&,
                                        bool,
                                        gfx::Texture*,
                                        float) noexcept;
static_assert(std::is_same_v<
              decltype(static_cast<BeginRenderingFunction>(&gfx::begin_rendering)),
              BeginRenderingFunction>);

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
    const TypedAllocation& allocation)
{
    const gfx::GpuAllocation<std::uint32_t> typed =
        gfx::gpu_malloc<std::uint32_t>(
            device, 4, gfx::MemoryType::default_);
    gfx::gpu_free(device, allocation);
    const gfx::GpuRange range = gfx::gpu_range(typed);
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
