#pragma once

#include <clean_gfx/shader_types.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace gfx
{

namespace detail
{
struct DeviceState;
}

enum class Error : std::uint8_t
{
    none,
    unsupported,
    out_of_device_memory,
    out_of_descriptors,
    invalid_shader,
    device_lost,
    driver_error,
};

enum class MemoryType : std::uint8_t
{
    upload,
    gpu,
    readback,
};

struct GpuRange
{
    void* gpu_ptr;
    std::uint64_t size;
};

struct GpuAllocation
{
    void* cpu_ptr;
    void* gpu_ptr;
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
    std::uint32_t texture_capacity = 4096;
    std::uint32_t sampler_capacity = 256;
    bool enable_validation = true;
};

struct DeviceCaps
{
    std::string device_name;
    std::uint32_t api_version = 0;
    std::uint32_t max_push_data_size = 0;
    std::uint64_t image_descriptor_size = 0;
    std::uint64_t sampler_descriptor_size = 0;
    std::uint32_t first_texture_index = 0;
    std::uint32_t first_sampler_index = 0;
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

class Device;
class CommandList;

class Texture
{
public:
    Texture() noexcept;
    ~Texture() noexcept;
    Texture(Texture&&) noexcept;
    Texture& operator=(Texture&&) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] std::uint32_t sampled_index() const noexcept;
    [[nodiscard]] std::uint32_t storage_index() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Texture(std::shared_ptr<Impl> impl) noexcept;
    friend class Device;
    friend class CommandList;
};

class Sampler
{
public:
    Sampler() noexcept;
    ~Sampler() noexcept;
    Sampler(Sampler&&) noexcept;
    Sampler& operator=(Sampler&&) noexcept;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    [[nodiscard]] std::uint32_t index() const noexcept;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Sampler(std::unique_ptr<Impl> impl) noexcept;
    friend class Device;
};

class Pipeline
{
public:
    Pipeline() noexcept;
    ~Pipeline() noexcept;
    Pipeline(Pipeline&&) noexcept;
    Pipeline& operator=(Pipeline&&) noexcept;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Pipeline(std::shared_ptr<Impl> impl) noexcept;
    friend class Device;
    friend class CommandList;
};

class CommandList
{
public:
    CommandList() noexcept;
    ~CommandList() noexcept;
    CommandList(CommandList&&) noexcept;
    CommandList& operator=(CommandList&&) noexcept;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    void bind_pipeline(const Pipeline& pipeline) noexcept;
    void push_data(std::span<const std::byte> bytes, std::uint32_t offset = 0) noexcept;

    template<typename T>
    void push_root(const T& value) noexcept
    {
        static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
                      "root data must be a standard-layout, trivially copyable POD");
        static_assert(sizeof(T) % 4 == 0,
                      "vkCmdPushDataEXT requires a root size that is a multiple of four");
        push_data(std::as_bytes(std::span{&value, std::size_t{1}}));
    }

    void begin_rendering(Texture& color,
                         float4 clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
                         bool clear = true,
                         Texture* depth = nullptr,
                         float clear_depth = 1.0f) noexcept;
    void end_rendering() noexcept;

    void draw(std::uint32_t vertex_count,
              std::uint32_t instance_count = 1,
              std::uint32_t first_vertex = 0,
              std::uint32_t first_instance = 0) noexcept;
    void draw_indexed(GpuRange indices,
                      IndexType type,
                      std::uint32_t index_count,
                      std::uint32_t instance_count = 1,
                      std::uint32_t first_index = 0,
                      std::int32_t vertex_offset = 0,
                      std::uint32_t first_instance = 0) noexcept;
    void draw_indirect(GpuRange arguments,
                       std::uint32_t draw_count = 1,
                       std::uint32_t stride = 0) noexcept;
    void draw_indexed_indirect(GpuRange indices,
                               IndexType type,
                               GpuRange arguments,
                               std::uint32_t draw_count = 1,
                               std::uint32_t stride = 0) noexcept;
    void dispatch(std::uint32_t x, std::uint32_t y = 1,
                  std::uint32_t z = 1) noexcept;
    void dispatch_indirect(GpuRange arguments) noexcept;

    void copy_memory(GpuRange source, GpuRange destination) noexcept;
    void copy_memory_to_texture(GpuRange source, Texture& destination) noexcept;
    void copy_texture_to_memory(Texture& source, GpuRange destination) noexcept;

    void barrier(Stage before, Access before_access,
                 Stage after, Access after_access) noexcept;
    [[nodiscard]] Error finish() noexcept;
    explicit operator bool() const noexcept;

private:
    struct AddressRange;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit CommandList(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] AddressRange validate_and_retain(
        GpuRange range) noexcept;
    void retain(Texture& texture) noexcept;
    [[nodiscard]] bool require_graphics_pipeline() const noexcept;
    [[nodiscard]] bool require_compute_pipeline() const noexcept;
    friend class Device;
};

class Device
{
public:
    Device() noexcept;
    ~Device() noexcept;
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] static Error create(Device& output,
                                      const DeviceDesc& desc = {}) noexcept;
    [[nodiscard]] const DeviceCaps& caps() const noexcept;

    [[nodiscard]] GpuAllocation gpu_malloc(
        std::uint64_t byte_count,
        MemoryType memory = MemoryType::upload,
        std::uint64_t alignment = 16) const noexcept;

    template<typename T>
    [[nodiscard]] GpuAllocation gpu_malloc(
        std::size_t count = 1,
        MemoryType memory = MemoryType::upload) const noexcept
    {
        static_assert(std::is_object_v<T>, "gpu_malloc<T> requires an object type");
        if (count > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
        {
            assert(false && "gpu_malloc size overflow");
            std::abort();
        }
        constexpr std::uint64_t alignment = alignof(T) > 16 ? alignof(T) : 16;
        return gpu_malloc(
            static_cast<std::uint64_t>(count) * sizeof(T), memory, alignment);
    }

    void gpu_free(GpuAllocation allocation) const noexcept;

    [[nodiscard]] Error create_texture(Texture& output,
                                       const TextureDesc& desc) const noexcept;
    [[nodiscard]] Error create_sampler(Sampler& output,
                                       const SamplerDesc& desc = {}) const noexcept;
    [[nodiscard]] Error create_graphics_pipeline(
        Pipeline& output, const GraphicsPipelineDesc& desc) const noexcept;
    [[nodiscard]] Error create_compute_pipeline(
        Pipeline& output, const ComputePipelineDesc& desc) const noexcept;
    [[nodiscard]] Error begin_commands(CommandList& output) const noexcept;
    [[nodiscard]] Error submit_and_wait(CommandList&& commands) const noexcept;
    [[nodiscard]] Error wait_idle() const noexcept;
    explicit operator bool() const noexcept;

private:
    std::shared_ptr<detail::DeviceState> impl_;
    explicit Device(std::shared_ptr<detail::DeviceState> impl) noexcept;
};

} // namespace gfx
