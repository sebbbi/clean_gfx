#pragma once

#include <clean_gfx/shader_types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace clean_gfx
{

namespace detail
{
struct DeviceState;
struct BufferAllocation;
}

using DeviceAddress = std::uint64_t;

class Error final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

enum class MemoryType : std::uint8_t
{
    upload,
    gpu,
    readback,
};

enum class Format : std::uint8_t
{
    rgba8_unorm,
    bgra8_unorm,
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

enum class ImageState : std::uint8_t
{
    undefined,
    transfer_source,
    transfer_destination,
    shader_read,
    storage,
    color_attachment,
    depth_attachment,
    present,
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
    std::string application_name = "clean_gfx application";
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

struct BufferDesc
{
    std::uint64_t size = 0;
    MemoryType memory = MemoryType::upload;
    std::string_view name;
};

class BufferSlice
{
public:
    BufferSlice() noexcept = default;

    [[nodiscard]] BufferSlice subspan(std::uint64_t offset,
                                      std::uint64_t byte_count) const;

    [[nodiscard]] DeviceAddress address() const noexcept { return address_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    explicit operator bool() const noexcept { return allocation_ != nullptr; }

private:
    DeviceAddress address_ = 0;
    std::uint64_t size_ = 0;
    std::shared_ptr<detail::BufferAllocation> allocation_;

    BufferSlice(DeviceAddress address,
                std::uint64_t size,
                std::shared_ptr<detail::BufferAllocation> allocation) noexcept;
    friend class Buffer;
    friend class CommandList;
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

class Buffer
{
public:
    Buffer() noexcept;
    ~Buffer();
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] DeviceAddress address() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] void* mapped_data() noexcept;
    [[nodiscard]] const void* mapped_data() const noexcept;
    [[nodiscard]] BufferSlice slice(std::uint64_t offset = 0,
                                    std::uint64_t byte_count = 0) const;

    void flush(std::uint64_t offset = 0, std::uint64_t byte_count = 0) const;
    void invalidate(std::uint64_t offset = 0, std::uint64_t byte_count = 0) const;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Buffer(std::unique_ptr<Impl> impl) noexcept;
    friend class Device;
};

class Texture
{
public:
    Texture() noexcept;
    ~Texture();
    Texture(Texture&&) noexcept;
    Texture& operator=(Texture&&) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] std::uint32_t sampled_index() const;
    [[nodiscard]] std::uint32_t storage_index() const;
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
    ~Sampler();
    Sampler(Sampler&&) noexcept;
    Sampler& operator=(Sampler&&) noexcept;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    [[nodiscard]] std::uint32_t index() const;
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
    ~Pipeline();
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
    ~CommandList();
    CommandList(CommandList&&) noexcept;
    CommandList& operator=(CommandList&&) noexcept;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    void bind_pipeline(const Pipeline& pipeline);
    void push_data(std::span<const std::byte> bytes, std::uint32_t offset = 0);

    template<typename T>
    void push_root(const T& value)
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
                         float clear_depth = 1.0f);
    void end_rendering();

    void draw(std::uint32_t vertex_count,
              std::uint32_t instance_count = 1,
              std::uint32_t first_vertex = 0,
              std::uint32_t first_instance = 0);
    void draw_indexed(BufferSlice indices,
                      IndexType type,
                      std::uint32_t index_count,
                      std::uint32_t instance_count = 1,
                      std::uint32_t first_index = 0,
                      std::int32_t vertex_offset = 0,
                      std::uint32_t first_instance = 0);
    void draw_indirect(BufferSlice arguments,
                       std::uint32_t draw_count = 1,
                       std::uint32_t stride = 0);
    void draw_indexed_indirect(BufferSlice indices,
                               IndexType type,
                               BufferSlice arguments,
                               std::uint32_t draw_count = 1,
                               std::uint32_t stride = 0);
    void dispatch(std::uint32_t x, std::uint32_t y = 1, std::uint32_t z = 1);
    void dispatch_indirect(BufferSlice arguments);

    void copy_buffer(BufferSlice source, BufferSlice destination,
                     std::uint64_t byte_count = 0);
    void copy_buffer_to_texture(BufferSlice source, Texture& destination);
    void copy_texture_to_buffer(Texture& source, BufferSlice destination);

    void transition(Texture& texture, ImageState before, ImageState after);
    void barrier(Stage before, Access before_access,
                 Stage after, Access after_access);
    void finish();
    explicit operator bool() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit CommandList(std::unique_ptr<Impl> impl) noexcept;
    void validate_and_retain(const BufferSlice& slice);
    void retain(Texture& texture);
    void require_graphics_pipeline() const;
    void require_compute_pipeline() const;
    friend class Device;
};

class Device
{
public:
    Device() noexcept;
    ~Device();
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] static Device create(const DeviceDesc& desc = {});
    [[nodiscard]] const DeviceCaps& caps() const;

    [[nodiscard]] Buffer create_buffer(const BufferDesc& desc) const;
    [[nodiscard]] Texture create_texture(const TextureDesc& desc) const;
    [[nodiscard]] Sampler create_sampler(const SamplerDesc& desc = {}) const;
    [[nodiscard]] Pipeline create_graphics_pipeline(
        const GraphicsPipelineDesc& desc) const;
    [[nodiscard]] Pipeline create_compute_pipeline(
        const ComputePipelineDesc& desc) const;
    [[nodiscard]] CommandList begin_commands() const;
    void submit_and_wait(CommandList&& commands) const;
    void wait_idle() const;
    explicit operator bool() const noexcept;

private:
    std::shared_ptr<detail::DeviceState> impl_;
    explicit Device(std::shared_ptr<detail::DeviceState> impl) noexcept;
};

} // namespace clean_gfx
