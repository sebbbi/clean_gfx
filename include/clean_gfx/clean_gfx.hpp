#pragma once

#include <clean_gfx/shader_types.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace gfx
{

struct Device;
struct Texture;
struct Pipeline;
struct CommandList;

enum class Error : std::uint8_t
{
    none,
    unsupported,
    out_of_device_memory,
    device_lost,
    driver_error,
};

enum class MemoryType : std::uint8_t
{
    default_,
    gpu_only,
    readback,
};

struct GpuRange
{
    void* gpu;
    std::uint64_t size;
};

template<typename T = std::byte>
struct GpuAllocation
{
    T* cpu;
    T* gpu;
    std::uint64_t size;
};

enum class Format : std::uint8_t
{
    rgba8_unorm,
    rgba8_srgb,
    bgra8_unorm,
    bgra8_srgb,
    rgba16_float,
    rgba32_float,
    d32_float,
};

enum class TextureUsage : std::uint32_t
{
    none = 0,
    sampled = 1u << 0u,
    storage = 1u << 1u,
    color_attachment = 1u << 2u,
    depth_attachment = 1u << 3u,
    transfer_source = 1u << 4u,
    transfer_destination = 1u << 5u,
};

enum class TextureDescriptorType : std::uint8_t
{
    sampled,
    storage,
};

constexpr TextureUsage operator|(TextureUsage lhs, TextureUsage rhs) noexcept
{
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(lhs) |
                                     static_cast<std::uint32_t>(rhs));
}

constexpr TextureUsage operator&(TextureUsage lhs, TextureUsage rhs) noexcept
{
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(lhs) &
                                     static_cast<std::uint32_t>(rhs));
}

enum class Filter : std::uint8_t
{
    nearest,
    linear,
};

enum class AddressMode : std::uint8_t
{
    repeat,
    mirrored_repeat,
    clamp_to_edge,
};

enum class PrimitiveTopology : std::uint8_t
{
    triangle_list,
    triangle_strip,
    line_list,
};

enum class CullMode : std::uint8_t
{
    none,
    clockwise,
    counter_clockwise,
};

enum class IndexType : std::uint8_t
{
    uint16,
    uint32,
};

enum class Stage : std::uint64_t
{
    none = 0,
    transfer = 1ull << 0u,
    vertex = 1ull << 1u,
    fragment = 1ull << 2u,
    compute = 1ull << 3u,
    color_output = 1ull << 4u,
    host = 1ull << 5u,
    indirect = 1ull << 6u,
    index_input = 1ull << 7u,
    depth_tests = 1ull << 8u,
    all = ~0ull,
};

constexpr Stage operator|(Stage lhs, Stage rhs) noexcept
{
    return static_cast<Stage>(static_cast<std::uint64_t>(lhs) |
                              static_cast<std::uint64_t>(rhs));
}

enum class Access : std::uint64_t
{
    none = 0,
    transfer_read = 1ull << 0u,
    transfer_write = 1ull << 1u,
    shader_read = 1ull << 2u,
    shader_write = 1ull << 3u,
    color_read = 1ull << 4u,
    color_write = 1ull << 5u,
    depth_read = 1ull << 6u,
    depth_write = 1ull << 7u,
    indirect_read = 1ull << 8u,
    index_read = 1ull << 9u,
    host_read = 1ull << 10u,
    host_write = 1ull << 11u,
    descriptor_read = 1ull << 12u,
};

constexpr Access operator|(Access lhs, Access rhs) noexcept
{
    return static_cast<Access>(static_cast<std::uint64_t>(lhs) |
                               static_cast<std::uint64_t>(rhs));
}

struct DeviceDesc
{
    const char* application_name = "clean_gfx application";
};

struct DeviceCaps
{
    const char* device_name;
    std::uint32_t api_version;
    std::uint64_t max_push_data_size;
    std::uint64_t image_descriptor_size;
    std::uint64_t sampler_descriptor_size;
};

struct DeviceInit
{
    Device* device;
    Error error;
};

struct TextureDesc
{
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;
    std::uint32_t mip_levels = 1;
    Format format = Format::rgba8_unorm;
    TextureUsage usage = TextureUsage::sampled;
    std::string_view name;
};

struct TextureViewDesc
{
    std::uint32_t base_mip = 0;
    std::uint32_t mip_count = 0; // Zero selects every remaining mip level.
};

struct SamplerDesc
{
    Filter min_filter = Filter::linear;
    Filter mag_filter = Filter::linear;
    AddressMode address_u = AddressMode::repeat;
    AddressMode address_v = AddressMode::repeat;
    AddressMode address_w = AddressMode::repeat;
};

struct GraphicsPipelineDesc
{
    std::span<const std::uint32_t> vertex_spirv;
    std::span<const std::uint32_t> fragment_spirv;
    Format color_format = Format::rgba8_unorm;
    Format depth_format = Format::d32_float;
    bool depth_enabled = false;
    bool depth_write = false;
    PrimitiveTopology topology = PrimitiveTopology::triangle_list;
    CullMode cull = CullMode::none;
    std::string_view name;
};

struct ComputePipelineDesc
{
    std::span<const std::uint32_t> compute_spirv;
    std::string_view name;
};

[[nodiscard]] DeviceInit create_device(const DeviceDesc& desc = {}) noexcept;
void destroy_device(Device* device) noexcept;
[[nodiscard]] DeviceCaps get_device_caps(const Device* device) noexcept;

[[nodiscard]] GpuAllocation<> gpu_malloc(
    Device* device,
    std::uint64_t byte_count,
    MemoryType memory = MemoryType::default_,
    std::uint64_t alignment = 16) noexcept;

template<typename T>
[[nodiscard]] GpuAllocation<T> gpu_malloc(
    Device* device,
    std::size_t count = 1,
    MemoryType memory = MemoryType::default_) noexcept
{
    static_assert(std::is_object_v<T>, "gpu_malloc<T> requires an object type");
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
                  "gpu_malloc<T> requires a mutable object type");
    if (count > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
    {
        assert(false && "gpu_malloc size overflow");
        std::abort();
    }
    constexpr std::uint64_t alignment = alignof(T) > 16 ? alignof(T) : 16;
    const GpuAllocation<> bytes = gpu_malloc(
        device, static_cast<std::uint64_t>(count) * sizeof(T), memory, alignment);
    return {
        .cpu = reinterpret_cast<T*>(bytes.cpu),
        .gpu = reinterpret_cast<T*>(bytes.gpu),
        .size = bytes.size,
    };
}

[[nodiscard]] GpuAllocation<> gpu_malloc_resource_heap(
    Device* device, std::uint64_t byte_count) noexcept;
[[nodiscard]] GpuAllocation<> gpu_malloc_sampler_heap(
    Device* device, std::uint64_t byte_count) noexcept;
void gpu_free(Device* device, GpuAllocation<> allocation) noexcept;

template<typename T>
void gpu_free(Device* device, GpuAllocation<T> allocation) noexcept
{
    gpu_free(device, {
        .cpu = reinterpret_cast<std::byte*>(allocation.cpu),
        .gpu = reinterpret_cast<std::byte*>(allocation.gpu),
        .size = allocation.size,
    });
}

template<typename T>
[[nodiscard]] constexpr GpuRange gpu_range(GpuAllocation<T> allocation) noexcept
{
    return {.gpu = static_cast<void*>(allocation.gpu), .size = allocation.size};
}

[[nodiscard]] Texture* create_texture(Device* device,
                                      const TextureDesc& desc) noexcept;
void destroy_texture(Texture* texture) noexcept;
[[nodiscard]] std::uint32_t texture_width(const Texture* texture) noexcept;
[[nodiscard]] std::uint32_t texture_height(const Texture* texture) noexcept;
void write_texture_descriptor(Device* device,
                              void* cpu_destination,
                              const Texture* texture,
                              TextureDescriptorType type,
                              const TextureViewDesc& view = {}) noexcept;
void write_sampler_descriptor(Device* device,
                              void* cpu_destination,
                              const SamplerDesc& desc = {}) noexcept;

[[nodiscard]] Pipeline* create_graphics_pipeline(
    Device* device, const GraphicsPipelineDesc& desc) noexcept;
[[nodiscard]] Pipeline* create_compute_pipeline(
    Device* device, const ComputePipelineDesc& desc) noexcept;
void destroy_pipeline(Pipeline* pipeline) noexcept;

[[nodiscard]] CommandList* begin_commands(Device* device) noexcept;
void destroy_command_list(CommandList* commands) noexcept;
void submit(Device* device, CommandList* commands) noexcept;
void submit_and_wait(Device* device, CommandList* commands) noexcept;
void wait_idle(Device* device) noexcept;

void bind_pipeline(CommandList* commands, const Pipeline* pipeline) noexcept;
void set_resource_heap(CommandList* commands, GpuRange heap) noexcept;
void set_sampler_heap(CommandList* commands, GpuRange heap) noexcept;
void begin_rendering(CommandList* commands,
                     Texture* color,
                     float4 clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
                     bool clear = true,
                     Texture* depth = nullptr,
                     float clear_depth = 1.0f) noexcept;
void end_rendering(CommandList* commands) noexcept;
void copy_memory(CommandList* commands,
                 GpuRange source,
                 GpuRange destination) noexcept;
void copy_memory_to_texture(CommandList* commands,
                            GpuRange source,
                            Texture* destination) noexcept;
void copy_texture_to_memory(CommandList* commands,
                            Texture* source,
                            GpuRange destination) noexcept;
void barrier(CommandList* commands,
             Stage before,
             Access before_access,
             Stage after,
             Access after_access) noexcept;

namespace detail
{

template<typename Root>
[[nodiscard]] consteval std::size_t root_data_size() noexcept
{
    static_assert(std::is_standard_layout_v<Root> &&
                      std::is_trivially_copyable_v<Root>,
                  "root data must be a standard-layout, trivially copyable type");
    static_assert(sizeof(Root) % 4 == 0,
                  "root data size must be a multiple of four bytes");
    return sizeof(Root);
}

void draw_impl(CommandList* commands,
               const void* root,
               std::size_t root_size,
               std::uint32_t vertex_count,
               std::uint32_t instance_count,
               std::uint32_t first_vertex,
               std::uint32_t first_instance) noexcept;
void draw_indexed_impl(CommandList* commands,
                       const void* root,
                       std::size_t root_size,
                       GpuRange indices,
                       IndexType type,
                       std::uint32_t index_count,
                       std::uint32_t instance_count,
                       std::uint32_t first_index,
                       std::int32_t vertex_offset,
                       std::uint32_t first_instance) noexcept;
void draw_indirect_impl(CommandList* commands,
                        const void* root,
                        std::size_t root_size,
                        GpuRange arguments,
                        std::uint32_t draw_count,
                        std::uint32_t stride) noexcept;
void draw_indexed_indirect_impl(CommandList* commands,
                                const void* root,
                                std::size_t root_size,
                                GpuRange indices,
                                IndexType type,
                                GpuRange arguments,
                                std::uint32_t draw_count,
                                std::uint32_t stride) noexcept;
void dispatch_impl(CommandList* commands,
                   const void* root,
                   std::size_t root_size,
                   std::uint32_t x,
                   std::uint32_t y,
                   std::uint32_t z) noexcept;
void dispatch_indirect_impl(CommandList* commands,
                            const void* root,
                            std::size_t root_size,
                            GpuRange arguments) noexcept;

} // namespace detail

template<typename Root>
void draw(CommandList* commands,
          const Root* root,
          std::uint32_t vertex_count,
          std::uint32_t instance_count = 1,
          std::uint32_t first_vertex = 0,
          std::uint32_t first_instance = 0) noexcept
{
    detail::draw_impl(commands, root, detail::root_data_size<Root>(), vertex_count,
                      instance_count, first_vertex, first_instance);
}

inline void draw(CommandList* commands,
                 std::nullptr_t,
                 std::uint32_t vertex_count,
                 std::uint32_t instance_count = 1,
                 std::uint32_t first_vertex = 0,
                 std::uint32_t first_instance = 0) noexcept
{
    detail::draw_impl(commands, nullptr, 0, vertex_count, instance_count,
                      first_vertex, first_instance);
}

template<typename Root>
void draw_indexed(CommandList* commands,
                  const Root* root,
                  GpuRange indices,
                  IndexType type,
                  std::uint32_t index_count,
                  std::uint32_t instance_count = 1,
                  std::uint32_t first_index = 0,
                  std::int32_t vertex_offset = 0,
                  std::uint32_t first_instance = 0) noexcept
{
    detail::draw_indexed_impl(commands, root, detail::root_data_size<Root>(),
                              indices, type, index_count, instance_count,
                              first_index, vertex_offset, first_instance);
}

inline void draw_indexed(CommandList* commands,
                         std::nullptr_t,
                         GpuRange indices,
                         IndexType type,
                         std::uint32_t index_count,
                         std::uint32_t instance_count = 1,
                         std::uint32_t first_index = 0,
                         std::int32_t vertex_offset = 0,
                         std::uint32_t first_instance = 0) noexcept
{
    detail::draw_indexed_impl(commands, nullptr, 0, indices, type, index_count,
                              instance_count, first_index, vertex_offset, first_instance);
}

template<typename Root>
void draw_indirect(CommandList* commands,
                   const Root* root,
                   GpuRange arguments,
                   std::uint32_t draw_count = 1,
                   std::uint32_t stride = 0) noexcept
{
    detail::draw_indirect_impl(commands, root, detail::root_data_size<Root>(),
                               arguments, draw_count, stride);
}

inline void draw_indirect(CommandList* commands,
                          std::nullptr_t,
                          GpuRange arguments,
                          std::uint32_t draw_count = 1,
                          std::uint32_t stride = 0) noexcept
{
    detail::draw_indirect_impl(commands, nullptr, 0, arguments, draw_count, stride);
}

template<typename Root>
void draw_indexed_indirect(CommandList* commands,
                           const Root* root,
                           GpuRange indices,
                           IndexType type,
                           GpuRange arguments,
                           std::uint32_t draw_count = 1,
                           std::uint32_t stride = 0) noexcept
{
    detail::draw_indexed_indirect_impl(
        commands, root, detail::root_data_size<Root>(), indices, type,
        arguments, draw_count, stride);
}

inline void draw_indexed_indirect(CommandList* commands,
                                  std::nullptr_t,
                                  GpuRange indices,
                                  IndexType type,
                                  GpuRange arguments,
                                  std::uint32_t draw_count = 1,
                                  std::uint32_t stride = 0) noexcept
{
    detail::draw_indexed_indirect_impl(
        commands, nullptr, 0, indices, type, arguments, draw_count, stride);
}

template<typename Root>
void dispatch(CommandList* commands,
              const Root* root,
              std::uint32_t x,
              std::uint32_t y = 1,
              std::uint32_t z = 1) noexcept
{
    detail::dispatch_impl(commands, root, detail::root_data_size<Root>(), x, y, z);
}

inline void dispatch(CommandList* commands,
                     std::nullptr_t,
                     std::uint32_t x,
                     std::uint32_t y = 1,
                     std::uint32_t z = 1) noexcept
{
    detail::dispatch_impl(commands, nullptr, 0, x, y, z);
}

template<typename Root>
void dispatch_indirect(CommandList* commands,
                       const Root* root,
                       GpuRange arguments) noexcept
{
    detail::dispatch_indirect_impl(
        commands, root, detail::root_data_size<Root>(), arguments);
}

inline void dispatch_indirect(CommandList* commands,
                              std::nullptr_t,
                              GpuRange arguments) noexcept
{
    detail::dispatch_indirect_impl(commands, nullptr, 0, arguments);
}

} // namespace gfx
