#include <clean_gfx/clean_gfx.hpp>

#include <offsetAllocator.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace gfx
{
namespace
{

constexpr std::uint32_t allocation_heap_size = 256u * 1024u * 1024u;

[[nodiscard]] Error error_from_vk(VkResult result) noexcept
{
    switch (result)
    {
    case VK_SUCCESS: return Error::none;
    case VK_ERROR_OUT_OF_HOST_MEMORY: std::abort();
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return Error::out_of_device_memory;
    case VK_ERROR_DEVICE_LOST: return Error::device_lost;
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return Error::unsupported;
    case VK_ERROR_INVALID_SHADER_NV: return Error::invalid_shader;
    case VK_ERROR_TOO_MANY_OBJECTS: return Error::out_of_device_memory;
    default: return Error::driver_error;
    }
}

template<typename T>
T load_instance_proc(VkInstance instance, const char* name) noexcept
{
    const auto proc = vkGetInstanceProcAddr(instance, name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T load_device_proc(VkDevice device, const char* name) noexcept
{
    const auto proc = vkGetDeviceProcAddr(device, name);
    return reinterpret_cast<T>(proc);
}

template<typename T>
T align_up(T value, T alignment)
{
    assert(alignment != 0);
    assert(value <= std::numeric_limits<T>::max() - (alignment - 1));
    return ((value + alignment - 1) / alignment) * alignment;
}

template<typename T>
constexpr bool has_flag(T value, T flag)
{
    using U = std::underlying_type_t<T>;
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

bool has_name(std::span<const VkExtensionProperties> values, const char* name)
{
    return std::ranges::any_of(values, [name](const VkExtensionProperties& value) {
        return std::strcmp(value.extensionName, name) == 0;
    });
}

#if !defined(NDEBUG)
bool has_name(std::span<const VkLayerProperties> values, const char* name)
{
    return std::ranges::any_of(values, [name](const VkLayerProperties& value) {
        return std::strcmp(value.layerName, name) == 0;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callback_data &&
        callback_data->pMessage)
    {
        // Keep the library callback dependency-free. Applications can still install
        // their own messenger; this one makes validation failures debugger-visible.
        std::fputs("clean_gfx validation: ", stderr);
        std::fputs(callback_data->pMessage, stderr);
        std::fputc('\n', stderr);
    }
    return VK_FALSE;
}
#endif

VkFormat to_vk(Format format)
{
    switch (format)
    {
    case Format::rgba8_unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::rgba8_srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::bgra8_unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::bgra8_srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::rgba16_float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::rgba32_float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::d32_float: return VK_FORMAT_D32_SFLOAT;
    }
    assert(false && "unknown clean_gfx format");
    return VK_FORMAT_UNDEFINED;
}

std::uint64_t bytes_per_texel(Format format)
{
    switch (format)
    {
    case Format::rgba8_unorm:
    case Format::rgba8_srgb:
    case Format::bgra8_unorm:
    case Format::bgra8_srgb:
    case Format::d32_float: return 4;
    case Format::rgba16_float: return 8;
    case Format::rgba32_float: return 16;
    }
    assert(false && "unknown clean_gfx format");
    return 0;
}

std::uint64_t base_level_byte_size(const TextureDesc& desc)
{
    std::uint64_t size = bytes_per_texel(desc.format);
    for (const auto dimension : {desc.width, desc.height, desc.depth})
    {
        assert(dimension == 0 || size <= std::numeric_limits<std::uint64_t>::max() / dimension);
        size *= dimension;
    }
    return size;
}

std::uint64_t image_resource_byte_size(const TextureDesc& desc)
{
    std::uint64_t total = 0;
    auto width = desc.width;
    auto height = desc.height;
    auto depth = desc.depth;
    for (std::uint32_t mip = 0; mip < desc.mip_levels; ++mip)
    {
        std::uint64_t level_size = bytes_per_texel(desc.format);
        for (const auto dimension : {width, height, depth})
        {
            assert(level_size <= std::numeric_limits<std::uint64_t>::max() / dimension);
            level_size *= dimension;
        }
        assert(total <= std::numeric_limits<std::uint64_t>::max() - level_size);
        total += level_size;
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
        depth = std::max(depth / 2, 1u);
    }
    return total;
}

VkFormatFeatureFlags2 required_format_features(TextureUsage usage)
{
    VkFormatFeatureFlags2 result = 0;
    if (has_flag(usage, TextureUsage::sampled))
        result |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (has_flag(usage, TextureUsage::storage))
        result |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if (has_flag(usage, TextureUsage::color_attachment))
        result |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::depth_attachment))
        result |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(usage, TextureUsage::transfer_source))
        result |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (has_flag(usage, TextureUsage::transfer_destination))
        result |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    return result;
}

VkFormatFeatureFlags2 optimal_format_features(VkPhysicalDevice physical_device, Format format)
{
    VkFormatProperties3 properties3{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
    };
    VkFormatProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &properties3,
    };
    vkGetPhysicalDeviceFormatProperties2(physical_device, to_vk(format), &properties2);
    return properties3.optimalTilingFeatures;
}

VkFilter to_vk(Filter filter)
{
    switch (filter)
    {
    case Filter::nearest: return VK_FILTER_NEAREST;
    case Filter::linear: return VK_FILTER_LINEAR;
    }
    assert(false && "unknown texture filter");
    return VK_FILTER_NEAREST;
}

VkSamplerAddressMode to_vk(AddressMode mode)
{
    switch (mode)
    {
    case AddressMode::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::clamp_to_edge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    assert(false && "unknown sampler address mode");
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkPrimitiveTopology to_vk(PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::triangle_list: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::line_list: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    assert(false && "unknown primitive topology");
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags to_vk(CullMode cull)
{
    switch (cull)
    {
    case CullMode::none: return VK_CULL_MODE_NONE;
    case CullMode::clockwise: return VK_CULL_MODE_BACK_BIT;
    case CullMode::counter_clockwise: return VK_CULL_MODE_BACK_BIT;
    }
    assert(false && "unknown cull mode");
    return VK_CULL_MODE_NONE;
}

VkPipelineStageFlags2 to_vk(Stage stages)
{
    constexpr auto known_bits = static_cast<std::uint64_t>(Stage::transfer) |
                                static_cast<std::uint64_t>(Stage::vertex) |
                                static_cast<std::uint64_t>(Stage::fragment) |
                                static_cast<std::uint64_t>(Stage::compute) |
                                static_cast<std::uint64_t>(Stage::color_output) |
                                static_cast<std::uint64_t>(Stage::host) |
                                static_cast<std::uint64_t>(Stage::indirect) |
                                static_cast<std::uint64_t>(Stage::index_input) |
                                static_cast<std::uint64_t>(Stage::depth_tests);
    const auto bits = static_cast<std::uint64_t>(stages);
    const bool valid_bits = stages == Stage::all || (bits & ~known_bits) == 0;
    assert(valid_bits && "pipeline stage mask contains unknown bits");
    if (!valid_bits)
        return 0;
    if (stages == Stage::all)
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkPipelineStageFlags2 result = 0;
    if (has_flag(stages, Stage::transfer)) result |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if (has_flag(stages, Stage::vertex)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (has_flag(stages, Stage::fragment)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (has_flag(stages, Stage::compute)) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (has_flag(stages, Stage::color_output))
        result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (has_flag(stages, Stage::host)) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (has_flag(stages, Stage::indirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (has_flag(stages, Stage::index_input)) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (has_flag(stages, Stage::depth_tests))
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    return result;
}

VkAccessFlags2 to_vk(Access accesses)
{
    constexpr auto known_bits = static_cast<std::uint64_t>(Access::transfer_read) |
                                static_cast<std::uint64_t>(Access::transfer_write) |
                                static_cast<std::uint64_t>(Access::shader_read) |
                                static_cast<std::uint64_t>(Access::shader_write) |
                                static_cast<std::uint64_t>(Access::color_read) |
                                static_cast<std::uint64_t>(Access::color_write) |
                                static_cast<std::uint64_t>(Access::depth_read) |
                                static_cast<std::uint64_t>(Access::depth_write) |
                                static_cast<std::uint64_t>(Access::indirect_read) |
                                static_cast<std::uint64_t>(Access::index_read) |
                                static_cast<std::uint64_t>(Access::host_read) |
                                static_cast<std::uint64_t>(Access::host_write) |
                                static_cast<std::uint64_t>(Access::descriptor_read);
    const bool valid_bits = (static_cast<std::uint64_t>(accesses) & ~known_bits) == 0;
    assert(valid_bits && "access mask contains unknown bits");
    if (!valid_bits)
        return 0;
    VkAccessFlags2 result = 0;
    if (has_flag(accesses, Access::transfer_read)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (has_flag(accesses, Access::transfer_write)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (has_flag(accesses, Access::shader_read))
        result |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (has_flag(accesses, Access::shader_write)) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (has_flag(accesses, Access::color_read))
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::color_write))
        result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::depth_read))
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (has_flag(accesses, Access::depth_write))
        result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (has_flag(accesses, Access::indirect_read)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (has_flag(accesses, Access::index_read)) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (has_flag(accesses, Access::host_read)) result |= VK_ACCESS_2_HOST_READ_BIT;
    if (has_flag(accesses, Access::host_write)) result |= VK_ACCESS_2_HOST_WRITE_BIT;
    if (has_flag(accesses, Access::descriptor_read))
        result |= VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT |
                  VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
    return result;
}

void validate_stage_access(Stage stages, Access accesses)
{
    if (accesses == Access::none)
        return;
    assert(stages != Stage::none &&
           "a non-zero access mask requires a non-zero pipeline stage mask");
    if (stages == Stage::all)
        return;

    const auto has_shader_stage = has_flag(stages, Stage::vertex) ||
                                  has_flag(stages, Stage::fragment) ||
                                  has_flag(stages, Stage::compute);
    const auto require = [&](Access access, bool valid) {
        assert((!has_flag(accesses, access) || valid) &&
               "access is incompatible with the stage mask");
        (void)access;
        (void)valid;
    };
    const auto transfer_stage = has_flag(stages, Stage::transfer);
    require(Access::transfer_read, transfer_stage);
    require(Access::transfer_write, transfer_stage);
    require(Access::shader_read, has_shader_stage);
    require(Access::shader_write, has_shader_stage);
    require(Access::descriptor_read, has_shader_stage);
    require(Access::color_read, has_flag(stages, Stage::color_output));
    require(Access::color_write, has_flag(stages, Stage::color_output));
    require(Access::depth_read, has_flag(stages, Stage::depth_tests));
    require(Access::depth_write, has_flag(stages, Stage::depth_tests));
    require(Access::indirect_read, has_flag(stages, Stage::indirect));
    require(Access::index_read, has_flag(stages, Stage::index_input));
    require(Access::host_read, has_flag(stages, Stage::host));
    require(Access::host_write, has_flag(stages, Stage::host));
}

VkBufferUsageFlags universal_buffer_usage()
{
    return VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}

std::size_t memory_pool_index(MemoryType memory)
{
    switch (memory)
    {
    case MemoryType::default_: return 0;
    case MemoryType::gpu_only: return 1;
    case MemoryType::readback: return 2;
    default:
        assert(false && "invalid GPU memory type");
        return 0;
    }
}

constexpr VkMemoryPropertyFlags cpu_visible_memory_properties =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

constexpr VkMemoryPropertyFlags forbidden_memory_properties =
    VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
    VK_MEMORY_PROPERTY_PROTECTED_BIT |
    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;

bool is_usable_memory_type(const VkPhysicalDeviceMemoryProperties& properties,
                           std::uint32_t index)
{
    if (index >= properties.memoryTypeCount)
        return false;
    const auto& type = properties.memoryTypes[index];
    if ((type.propertyFlags & forbidden_memory_properties) != 0 ||
        type.heapIndex >= properties.memoryHeapCount)
    {
        return false;
    }
    return (properties.memoryHeaps[type.heapIndex].flags &
            VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) == 0;
}

bool has_cpu_visible_device_memory(const VkPhysicalDeviceMemoryProperties& properties)
{
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if (!is_usable_memory_type(properties, i))
            continue;
        const auto& type = properties.memoryTypes[i];
        if ((type.propertyFlags & cpu_visible_memory_properties) !=
            cpu_visible_memory_properties)
        {
            continue;
        }
        if (type.heapIndex < properties.memoryHeapCount &&
            (properties.memoryHeaps[type.heapIndex].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            properties.memoryHeaps[type.heapIndex].size >= allocation_heap_size)
        {
            return true;
        }
    }
    return false;
}

bool has_gpu_only_device_memory(const VkPhysicalDeviceMemoryProperties& properties)
{
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if (!is_usable_memory_type(properties, i))
            continue;
        const auto& type = properties.memoryTypes[i];
        const auto flags = type.propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 &&
            type.heapIndex < properties.memoryHeapCount &&
            (properties.memoryHeaps[type.heapIndex].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
            properties.memoryHeaps[type.heapIndex].size >= allocation_heap_size)
        {
            return true;
        }
    }
    return false;
}

constexpr VkAddressCommandFlagsKHR address_flags =
    VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR |
    VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR;

} // namespace

namespace detail
{

struct DeviceFunctions
{
    PFN_vkWriteSamplerDescriptorsEXT write_sampler_descriptors = nullptr;
    PFN_vkWriteResourceDescriptorsEXT write_resource_descriptors = nullptr;
    PFN_vkCmdBindSamplerHeapEXT cmd_bind_sampler_heap = nullptr;
    PFN_vkCmdBindResourceHeapEXT cmd_bind_resource_heap = nullptr;
    PFN_vkCmdPushDataEXT cmd_push_data = nullptr;
    PFN_vkCmdBindIndexBuffer3KHR cmd_bind_index_buffer = nullptr;
    PFN_vkCmdDrawIndirect2KHR cmd_draw_indirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect2KHR cmd_draw_indexed_indirect = nullptr;
    PFN_vkCmdDispatchIndirect2KHR cmd_dispatch_indirect = nullptr;
    PFN_vkCmdCopyMemoryKHR cmd_copy_memory = nullptr;
    PFN_vkCmdCopyMemoryToImageKHR cmd_copy_memory_to_image = nullptr;
    PFN_vkCmdCopyImageToMemoryKHR cmd_copy_image_to_memory = nullptr;
};

struct BackingBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    VkDeviceSize allocation_size = 0;
};

struct AllocationHeap;
struct AllocationRecord;
struct DescriptorAllocation;
struct ImageHeap;

enum class DescriptorHeapType : std::uint8_t
{
    resource,
    sampler,
};

struct TextureInitialization
{
    VkImage image = VK_NULL_HANDLE;
    VkImageAspectFlags aspect_mask = 0;
    std::uint32_t mip_levels = 0;
    bool initialized = false;
    std::vector<TextureInitialization*>* owner = nullptr;
};

struct FrameContext
{
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    void* command_list_storage = nullptr;
    std::uint64_t last_signal = 0;
    bool active = false;
};

struct DeviceState
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger = nullptr;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties physical_properties{};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    DeviceFunctions fn;
    DeviceCaps caps;
    mutable std::uint32_t live_memory_allocations = 0;
    std::array<std::vector<std::unique_ptr<AllocationHeap>>, 3> allocation_heaps;
    std::vector<AllocationRecord> allocations;
    std::vector<std::unique_ptr<DescriptorAllocation>> descriptor_allocations;
    std::vector<std::unique_ptr<ImageHeap>> image_heaps;
    std::vector<TextureInitialization*> pending_texture_initializations;
    std::array<FrameContext, 2> frames;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t next_frame = 0;
    std::uint64_t next_signal = 1;
    std::uint64_t last_submitted_signal = 0;

    ~DeviceState();

    DeviceState() = default;
    DeviceState(const DeviceState&) = delete;
    DeviceState& operator=(const DeviceState&) = delete;

    void destroy_backing(BackingBuffer& backing) const noexcept
    {
        if (!device)
            return;
        if (backing.mapped)
            vkUnmapMemory(device, backing.memory);
        if (backing.buffer)
            vkDestroyBuffer(device, backing.buffer, nullptr);
        if (backing.memory)
            free_memory(backing.memory);
        backing = {};
    }

    [[nodiscard]] Error allocate_memory(const VkMemoryAllocateInfo& info,
                                        VkDeviceMemory& output) const noexcept
    {
        output = VK_NULL_HANDLE;
        assert(info.memoryTypeIndex < memory_properties.memoryTypeCount);
        if (info.memoryTypeIndex >= memory_properties.memoryTypeCount)
            return Error::driver_error;
        const auto heap_index = memory_properties.memoryTypes[info.memoryTypeIndex].heapIndex;
        assert(heap_index < memory_properties.memoryHeapCount);
        if (heap_index >= memory_properties.memoryHeapCount)
            return Error::driver_error;
        if (info.allocationSize > memory_properties.memoryHeaps[heap_index].size)
            return Error::out_of_device_memory;

        const auto maximum = physical_properties.limits.maxMemoryAllocationCount;
        if (live_memory_allocations >= maximum)
            return Error::out_of_device_memory;
        ++live_memory_allocations;

        const auto result = vkAllocateMemory(device, &info, nullptr, &output);
        if (result != VK_SUCCESS)
        {
            --live_memory_allocations;
            output = VK_NULL_HANDLE;
            return error_from_vk(result);
        }
        return Error::none;
    }

    void free_memory(VkDeviceMemory memory) const noexcept
    {
        if (!memory)
            return;
        vkFreeMemory(device, memory, nullptr);
        assert(live_memory_allocations != 0);
        --live_memory_allocations;
    }

    [[nodiscard]] bool find_memory_type(std::uint32_t bits,
                                        VkMemoryPropertyFlags required,
                                        VkMemoryPropertyFlags preferred,
                                        VkDeviceSize minimum_heap_size,
                                        std::uint32_t& output,
                                        VkMemoryPropertyFlags forbidden = 0) const noexcept
    {
        std::optional<std::uint32_t> best;
        std::uint32_t best_score = 0;
        VkDeviceSize best_heap_size = 0;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) == 0)
                continue;
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required || (flags & forbidden) != 0)
                continue;
            if (!is_usable_memory_type(memory_properties, i))
                continue;
            const auto& heap = memory_properties.memoryHeaps[
                memory_properties.memoryTypes[i].heapIndex];
            if (heap.size < minimum_heap_size ||
                ((required & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                 (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0))
            {
                continue;
            }
            const auto score = static_cast<std::uint32_t>(std::popcount(flags & preferred));
            if (!best || score > best_score ||
                (score == best_score && heap.size > best_heap_size))
            {
                best = i;
                best_score = score;
                best_heap_size = heap.size;
            }
        }
        if (!best)
            return false;
        output = *best;
        return true;
    }

    [[nodiscard]] Error create_backing_buffer(
        BackingBuffer& output,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred,
        VkMemoryPropertyFlags forbidden = 0) const noexcept
    {
        output = {};
        assert(size != 0);
        if (size == 0 || size > vulkan13_properties.maxBufferSize)
            return Error::unsupported;
        BackingBuffer result;
        result.size = size;
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        auto error = error_from_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));
        if (error != Error::none)
            return error;

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        result.allocation_size = requirements.size;
        std::uint32_t memory_type = 0;
        if (!find_memory_type(
                requirements.memoryTypeBits, required, preferred,
                requirements.size, memory_type, forbidden))
        {
            destroy_backing(result);
            return Error::unsupported;
        }

        const VkMemoryDedicatedAllocateInfo dedicated_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = nullptr,
            .image = VK_NULL_HANDLE,
            .buffer = result.buffer,
        };
        const VkMemoryAllocateFlagsInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .pNext = &dedicated_info,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
            .deviceMask = 0,
        };
        const VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &flags_info,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        };
        error = allocate_memory(allocate_info, result.memory);
        if (error != Error::none)
        {
            destroy_backing(result);
            return error;
        }
        error = error_from_vk(vkBindBufferMemory(device, result.buffer, result.memory, 0));
        if (error != Error::none)
        {
            destroy_backing(result);
            return error;
        }

        if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            error = error_from_vk(
                vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));
            if (error != Error::none)
            {
                destroy_backing(result);
                return error;
            }
        }

        const VkBufferDeviceAddressInfo address_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .pNext = nullptr,
            .buffer = result.buffer,
        };
        result.address = vkGetBufferDeviceAddress(device, &address_info);
        if (result.address == 0)
        {
            destroy_backing(result);
            return Error::driver_error;
        }
        output = result;
        return Error::none;
    }

    [[nodiscard]] GpuAllocation allocate_gpu(
        VkDeviceSize size,
        MemoryType memory,
        VkDeviceSize alignment) noexcept;
    [[nodiscard]] GpuAllocation allocate_descriptor_heap(
        VkDeviceSize size,
        DescriptorHeapType type) noexcept;
    void release_allocation(GpuAllocation allocation) noexcept;
    [[nodiscard]] Error create_frame_commands(FrameContext& frame) noexcept;
    void destroy_frame_commands(FrameContext& frame) noexcept;
    [[nodiscard]] Error reset_frame_commands(FrameContext& frame) noexcept;

};

struct ImageHeap
{
    ImageHeap(DeviceState* owner, std::uint32_t type)
        : state(owner), memory_type(type), offsets(allocation_heap_size)
    {}

    ~ImageHeap()
    {
        if (state && memory)
            state->free_memory(memory);
    }

    DeviceState* state = nullptr;
    std::uint32_t memory_type = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    OffsetAllocator::Allocator offsets;
};

struct AllocationHeap
{
    AllocationHeap(DeviceState* owner, MemoryType type)
        : state(owner), memory(type), offsets(allocation_heap_size)
    {}

    ~AllocationHeap()
    {
        if (state)
            state->destroy_backing(backing);
    }

    DeviceState* state = nullptr;
    MemoryType memory;
    BackingBuffer backing;
    OffsetAllocator::Allocator offsets;
};

struct AllocationRecord
{
    AllocationHeap* heap = nullptr;
    OffsetAllocator::Allocation offset_allocation{};
    GpuAllocation value{};
};

struct DescriptorAllocation
{
    explicit DescriptorAllocation(DeviceState* owner) : state(owner) {}

    ~DescriptorAllocation()
    {
        if (state)
            state->destroy_backing(backing);
    }

    DeviceState* state = nullptr;
    BackingBuffer backing;
    GpuAllocation value{};
};

DeviceState::~DeviceState()
{
    if (device)
        vkDeviceWaitIdle(device);
    for (auto& frame : frames)
    {
        assert(!frame.active && "device destroyed while a command list is active");
        destroy_frame_commands(frame);
        if (frame.command_list_storage)
            ::operator delete(frame.command_list_storage);
        frame = {};
    }
    allocations.clear();
    for (auto& pool : allocation_heaps)
        pool.clear();
    descriptor_allocations.clear();
    image_heaps.clear();
    if (device && timeline)
        vkDestroySemaphore(device, timeline, nullptr);
    timeline = VK_NULL_HANDLE;
    if (device)
        vkDestroyDevice(device, nullptr);
    if (instance && debug_messenger && destroy_debug_messenger)
        destroy_debug_messenger(instance, debug_messenger, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
}

Error DeviceState::create_frame_commands(FrameContext& frame) noexcept
{
    assert(!frame.command_pool && !frame.command_buffer);
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    auto error = error_from_vk(
        vkCreateCommandPool(device, &pool_info, nullptr, &command_pool));
    if (error != Error::none)
        return error;
    frame.command_pool = command_pool;

    const VkCommandBufferAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = frame.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    error = error_from_vk(
        vkAllocateCommandBuffers(device, &allocate_info, &command_buffer));
    if (error != Error::none)
        destroy_frame_commands(frame);
    else
        frame.command_buffer = command_buffer;
    return error;
}

void DeviceState::destroy_frame_commands(FrameContext& frame) noexcept
{
    if (device && frame.command_pool)
        vkDestroyCommandPool(device, frame.command_pool, nullptr);
    frame.command_pool = VK_NULL_HANDLE;
    frame.command_buffer = VK_NULL_HANDLE;
}

Error DeviceState::reset_frame_commands(FrameContext& frame) noexcept
{
    if (!frame.command_pool)
    {
        assert(!frame.command_buffer);
        return Error::none;
    }
    assert(frame.command_buffer);
    const auto error = error_from_vk(vkResetCommandPool(device, frame.command_pool, 0));
    if (error != Error::none)
        destroy_frame_commands(frame);
    return error;
}

GpuAllocation DeviceState::allocate_gpu(VkDeviceSize size,
                                         MemoryType memory,
                                         VkDeviceSize alignment) noexcept
{
    assert(size != 0 && "gpu_malloc byte count must be non-zero");
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0 &&
           "gpu_malloc alignment must be a non-zero power of two");
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        return {};

    const bool request_fits = size <= allocation_heap_size && alignment != 0 &&
                               alignment - 1 <= allocation_heap_size - size;
    assert(request_fits && "gpu_malloc request including alignment padding exceeds 256 MiB");
    if (!request_fits)
        return {};

    const auto padded_size = static_cast<std::uint32_t>(size + alignment - 1);
    const auto pool_index = memory_pool_index(memory);
    const auto make_heap = [&]() -> std::unique_ptr<AllocationHeap> {
        auto heap = std::make_unique<AllocationHeap>(this, memory);
        VkMemoryPropertyFlags required = 0;
        VkMemoryPropertyFlags preferred = 0;
        VkMemoryPropertyFlags forbidden = 0;
        switch (memory)
        {
        case MemoryType::default_:
            required = cpu_visible_memory_properties;
            break;
        case MemoryType::gpu_only:
            required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            forbidden = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            break;
        case MemoryType::readback:
            required = cpu_visible_memory_properties;
            preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        default:
            assert(false && "gpu_malloc received an invalid memory type");
            return {};
        }
        if (create_backing_buffer(
                heap->backing,
                allocation_heap_size,
                universal_buffer_usage(),
                required,
                preferred,
                forbidden) != Error::none)
            return {};
        return heap;
    };

    const auto try_allocate = [&](AllocationHeap& heap, GpuAllocation& output) {
        if (heap.backing.mapped)
        {
            const auto host_base = reinterpret_cast<std::uintptr_t>(heap.backing.mapped);
            if ((host_base % alignment) != (heap.backing.address % alignment))
                return false;
        }

        const auto token = heap.offsets.allocate(padded_size);
        if (token.offset == OffsetAllocator::Allocation::NO_SPACE)
            return false;

        const auto raw_address = heap.backing.address + token.offset;
        const auto gpu_address = align_up(raw_address, alignment);
        const auto offset = gpu_address - heap.backing.address;
        const bool valid_range = offset <= allocation_heap_size &&
                                 size <= allocation_heap_size - offset;
        assert(valid_range && "OffsetAllocator returned an invalid padded range");
        if (!valid_range)
        {
            heap.offsets.free(token);
            return false;
        }

        void* cpu_pointer = nullptr;
        if (heap.backing.mapped)
        {
            const auto host_address =
                reinterpret_cast<std::uintptr_t>(heap.backing.mapped) + offset;
            assert(host_address % alignment == 0 &&
                   "mapped GPU allocation does not satisfy its requested alignment");
            if (host_address % alignment != 0)
            {
                heap.offsets.free(token);
                return false;
            }
            cpu_pointer = reinterpret_cast<void*>(host_address);
        }
        assert(gpu_address % alignment == 0 &&
               "GPU allocation does not satisfy its requested alignment");

        AllocationRecord record{
            .heap = &heap,
            .offset_allocation = token,
            .value = {
            .cpu_ptr = cpu_pointer,
            .gpu_ptr = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(gpu_address)),
            .size = size,
            },
        };

        output = record.value;
        allocations.push_back(record);
        return true;
    };

    auto& pool = allocation_heaps[pool_index];
    for (const auto& heap : pool)
    {
        GpuAllocation allocation{};
        if (try_allocate(*heap, allocation))
            return allocation;
    }

    auto heap = make_heap();
    if (!heap)
        return {};
    auto* heap_pointer = heap.get();
    pool.push_back(std::move(heap));
    GpuAllocation allocation{};
    if (try_allocate(*heap_pointer, allocation))
        return allocation;
    pool.pop_back();
    return {};
}

GpuAllocation DeviceState::allocate_descriptor_heap(
    VkDeviceSize size,
    DescriptorHeapType type) noexcept
{
    assert(size != 0 && "descriptor heap byte count must be non-zero");
    if (size == 0)
        return {};

    const bool resource = type == DescriptorHeapType::resource;
    const auto reserved_alignment = resource
        ? std::max(heap_properties.imageDescriptorAlignment,
                   heap_properties.bufferDescriptorAlignment)
        : heap_properties.samplerDescriptorAlignment;
    const auto heap_alignment = resource
        ? heap_properties.resourceHeapAlignment
        : heap_properties.samplerHeapAlignment;
    const auto reserved_size = resource
        ? heap_properties.minResourceHeapReservedRange
        : heap_properties.minSamplerHeapReservedRange;
    const auto maximum_size = resource
        ? heap_properties.maxResourceHeapSize
        : heap_properties.maxSamplerHeapSize;

    const bool valid_properties =
        reserved_alignment != 0 &&
        (reserved_alignment & (reserved_alignment - 1)) == 0 &&
        heap_alignment != 0 && (heap_alignment & (heap_alignment - 1)) == 0;
    assert(valid_properties && "descriptor heap alignment properties are invalid");
    if (!valid_properties || size > maximum_size ||
        size > std::numeric_limits<VkDeviceSize>::max() -
                   (reserved_alignment - 1))
    {
        return {};
    }

    const auto reserved_offset = align_up(size, reserved_alignment);
    if (reserved_offset > maximum_size ||
        reserved_size > maximum_size - reserved_offset)
    {
        return {};
    }
    const auto bind_size = reserved_offset + reserved_size;

    auto allocation = std::make_unique<DescriptorAllocation>(this);
    const auto error = create_backing_buffer(
        allocation->backing,
        bind_size,
        universal_buffer_usage() | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
        cpu_visible_memory_properties,
        0);
    if (error != Error::none)
        return {};

    const bool valid_backing = allocation->backing.mapped != nullptr &&
                               allocation->backing.address % heap_alignment == 0;
    assert(valid_backing && "descriptor heap backing address is invalid");
    if (!valid_backing)
        return {};

    allocation->value = {
        .cpu_ptr = allocation->backing.mapped,
        .gpu_ptr = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(allocation->backing.address)),
        .size = size,
    };
    const auto result = allocation->value;
    descriptor_allocations.push_back(std::move(allocation));
    return result;
}

void DeviceState::release_allocation(GpuAllocation allocation) noexcept
{
    if (!allocation.gpu_ptr)
    {
        assert(!allocation.cpu_ptr && allocation.size == 0 &&
               "gpu_free received an invalid empty allocation");
        return;
    }

    const auto entry = std::ranges::find_if(
        allocations,
        [&](const AllocationRecord& record) {
            return record.value.gpu_ptr == allocation.gpu_ptr;
        });
    if (entry != allocations.end())
    {
        const auto& record = *entry;
        const bool matches = record.value.cpu_ptr == allocation.cpu_ptr &&
                             record.value.gpu_ptr == allocation.gpu_ptr &&
                             record.value.size == allocation.size;
        assert(matches &&
               "gpu_free allocation fields do not match the original gpu_malloc result");
        if (!matches)
            return;
        assert(record.heap && "GPU allocation record has no owning heap");
        if (!record.heap)
            return;
        record.heap->offsets.free(record.offset_allocation);
        allocations.erase(entry);
        return;
    }

    const auto descriptor_entry = std::ranges::find_if(
        descriptor_allocations,
        [&](const std::unique_ptr<DescriptorAllocation>& record) {
            return record->value.gpu_ptr == allocation.gpu_ptr;
        });
    const bool found = descriptor_entry != descriptor_allocations.end();
    assert(found && "gpu_free requires a live allocation returned by a GPU allocator");
    if (!found)
        return;
    const auto& record = **descriptor_entry;
    const bool matches = record.value.cpu_ptr == allocation.cpu_ptr &&
                         record.value.gpu_ptr == allocation.gpu_ptr &&
                         record.value.size == allocation.size;
    assert(matches &&
           "gpu_free allocation fields do not match the original descriptor heap allocation");
    if (!matches)
        return;
    descriptor_allocations.erase(descriptor_entry);
}

} // namespace detail

struct Texture::Impl
{
    detail::DeviceState* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory dedicated_memory = VK_NULL_HANDLE;
    detail::ImageHeap* heap = nullptr;
    OffsetAllocator::Allocation heap_allocation{};
    VkImageView view = VK_NULL_HANDLE;
    TextureDesc desc;
    bool owns_heap_allocation = false;
    detail::TextureInitialization initialization;

    ~Impl()
    {
        if (!state)
            return;
        if (initialization.owner)
        {
            auto& owner = *initialization.owner;
            const auto entry = std::ranges::find(owner, &initialization);
            assert(entry != owner.end() &&
                   "texture initialization owner does not contain the texture");
            if (entry != owner.end())
                owner.erase(entry);
            initialization.owner = nullptr;
        }
        if (view)
            vkDestroyImageView(state->device, view, nullptr);
        if (image)
            vkDestroyImage(state->device, image, nullptr);
        if (owns_heap_allocation)
        {
            assert(heap);
            heap->offsets.free(heap_allocation);
        }
        if (dedicated_memory)
            state->free_memory(dedicated_memory);
    }
};

struct Pipeline::Impl
{
    detail::DeviceState* state = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    Format color_format = Format::rgba8_unorm;
    Format depth_format = Format::d32_float;
    bool depth_enabled = false;

    ~Impl()
    {
        if (state && pipeline)
            vkDestroyPipeline(state->device, pipeline, nullptr);
    }
};

struct CommandList::Impl
{
    detail::DeviceState* state = nullptr;
    detail::FrameContext* frame = nullptr;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool recording = true;
    bool rendering = false;
    Format rendering_color_format = Format::rgba8_unorm;
    Format rendering_depth_format = Format::d32_float;
    bool rendering_has_depth = false;
    Pipeline::Impl* bound_graphics = nullptr;
    Pipeline::Impl* bound_compute = nullptr;
    std::vector<detail::TextureInitialization*> pending_texture_initializations;

    ~Impl()
    {
        if (state && !pending_texture_initializations.empty())
        {
            for (auto* initialization : pending_texture_initializations)
            {
                if (!initialization)
                    continue;
                assert(!initialization->initialized);
                assert(initialization->owner == &pending_texture_initializations);
                initialization->owner = &state->pending_texture_initializations;
                state->pending_texture_initializations.push_back(initialization);
            }
            pending_texture_initializations.clear();
        }
        if (frame)
        {
            assert(frame->last_signal == 0);
            const auto error = state->reset_frame_commands(*frame);
            assert(error == Error::none &&
                   "failed to reset an abandoned command pool");
            (void)error;
            frame->active = false;
        }
    }

};

struct CommandList::AddressRange
{
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

namespace
{

template<typename CommandImplementation, typename TextureImplementation>
void initialize_texture(CommandImplementation& commands,
                         TextureImplementation& texture,
                         VkPipelineStageFlags2 destination_stages,
                         VkAccessFlags2 destination_access) noexcept
{
    auto& initialization = texture.initialization;
    if (initialization.initialized ||
        initialization.owner == &commands.pending_texture_initializations)
        return;

    const bool globally_pending =
        commands.state &&
        initialization.owner == &commands.state->pending_texture_initializations;
    assert(globally_pending &&
           "texture initialization belongs to another command list");
    if (!globally_pending)
        return;
    auto& global = commands.state->pending_texture_initializations;
    const auto pending_entry = std::ranges::find(global, &initialization);
    assert(pending_entry != global.end());
    if (pending_entry == global.end())
        return;
    global.erase(pending_entry);
    commands.pending_texture_initializations.push_back(&initialization);
    initialization.owner = &commands.pending_texture_initializations;

    const VkImageMemoryBarrier2 image_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask = destination_stages,
        .dstAccessMask = destination_access,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = initialization.image,
        .subresourceRange = {
            .aspectMask = initialization.aspect_mask,
            .baseMipLevel = 0,
            .levelCount = initialization.mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &image_barrier,
    };
    vkCmdPipelineBarrier2(commands.command_buffer, &dependency);
}

bool make_heap_bind_info(GpuRange heap,
                         VkDeviceSize heap_alignment,
                         VkDeviceSize reserved_alignment,
                         VkDeviceSize reserved_size,
                         VkDeviceSize maximum_size,
                         VkBindHeapInfoEXT& output) noexcept
{
    const bool nonempty = heap.gpu_ptr && heap.size != 0;
    assert(nonempty && "descriptor heap range must be non-empty");
    if (!nonempty || reserved_alignment == 0 || heap_alignment == 0 ||
        heap.size > std::numeric_limits<VkDeviceSize>::max() -
                        (reserved_alignment - 1))
    {
        return false;
    }
    const auto address = static_cast<VkDeviceAddress>(
        reinterpret_cast<std::uintptr_t>(heap.gpu_ptr));
    const auto reserved_offset = align_up<VkDeviceSize>(heap.size, reserved_alignment);
    const auto total_size = reserved_offset + reserved_size;
    const bool valid = address % heap_alignment == 0 &&
                       reserved_offset <= maximum_size &&
                       reserved_size <= maximum_size - reserved_offset &&
                       address <= std::numeric_limits<VkDeviceAddress>::max() - total_size;
    assert(valid &&
           "descriptor heap address, user size, or implementation reservation is invalid");
    if (!valid)
        return false;
    output = {
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext = nullptr,
        .heapRange = {address, total_size},
        .reservedRangeOffset = reserved_offset,
        .reservedRangeSize = reserved_size,
    };
    return true;
}

Error enumerate_device_extensions(VkPhysicalDevice physical_device,
                                  std::vector<VkExtensionProperties>& values) noexcept
{
    for (;;)
    {
        std::uint32_t count = 0;
        auto error = error_from_vk(
            vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr));
        if (error != Error::none)
            return error;
        values.resize(count);
        const auto result = vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        values.resize(count);
        return Error::none;
    }
}

#if !defined(NDEBUG)
Error enumerate_instance_extensions(std::vector<VkExtensionProperties>& values) noexcept
{
    for (;;)
    {
        std::uint32_t count = 0;
        auto error = error_from_vk(
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
        if (error != Error::none)
            return error;
        values.resize(count);
        const auto result =
            vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        values.resize(count);
        return Error::none;
    }
}

Error enumerate_instance_layers(std::vector<VkLayerProperties>& values) noexcept
{
    for (;;)
    {
        std::uint32_t count = 0;
        auto error = error_from_vk(vkEnumerateInstanceLayerProperties(&count, nullptr));
        if (error != Error::none)
            return error;
        values.resize(count);
        const auto result = vkEnumerateInstanceLayerProperties(&count, values.data());
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        values.resize(count);
        return Error::none;
    }
}
#endif

struct QueriedFeatures
{
    VkPhysicalDeviceFeatures2 core{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features vulkan11{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features vulkan14{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptor_heap{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR address_commands{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR};
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped_pointers{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified_image_layouts{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};

    QueriedFeatures()
    {
        core.pNext = &vulkan11;
        vulkan11.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        vulkan13.pNext = &vulkan14;
        vulkan14.pNext = &descriptor_heap;
        descriptor_heap.pNext = &address_commands;
        address_commands.pNext = &untyped_pointers;
        untyped_pointers.pNext = &unified_image_layouts;
    }
};

struct Candidate
{
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heap_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties vulkan13_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
};

Error inspect_candidate(VkPhysicalDevice physical_device, Candidate& output) noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    std::vector<VkExtensionProperties> extensions;
    const auto extension_error = enumerate_device_extensions(physical_device, extensions);
    if (extension_error != Error::none)
        return extension_error;
    constexpr std::array required_extensions{
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
    };
    for (const char* name : required_extensions)
    {
        if (!has_name(extensions, name))
            return Error::unsupported;
    }
    if (properties.apiVersion < VK_API_VERSION_1_4)
        return Error::unsupported;

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    if (!has_cpu_visible_device_memory(memory_properties))
        return Error::unsupported;
    if (!has_gpu_only_device_memory(memory_properties))
        return Error::unsupported;

    QueriedFeatures features;
    vkGetPhysicalDeviceFeatures2(physical_device, &features.core);
    const bool required_features =
        features.core.features.shaderInt16 == VK_TRUE &&
        features.core.features.fragmentStoresAndAtomics == VK_TRUE &&
        features.core.features.vertexPipelineStoresAndAtomics == VK_TRUE &&
        features.core.features.shaderStorageImageReadWithoutFormat == VK_TRUE &&
        features.core.features.shaderStorageImageWriteWithoutFormat == VK_TRUE &&
        features.core.features.multiDrawIndirect == VK_TRUE &&
        features.core.features.drawIndirectFirstInstance == VK_TRUE &&
        features.vulkan11.storageBuffer16BitAccess == VK_TRUE &&
        features.vulkan11.storagePushConstant16 == VK_TRUE &&
        features.vulkan11.shaderDrawParameters == VK_TRUE &&
        features.vulkan12.shaderFloat16 == VK_TRUE &&
        features.vulkan12.scalarBlockLayout == VK_TRUE &&
        features.vulkan12.bufferDeviceAddress == VK_TRUE &&
        features.vulkan12.timelineSemaphore == VK_TRUE &&
        features.vulkan13.synchronization2 == VK_TRUE &&
        features.vulkan13.dynamicRendering == VK_TRUE &&
        features.vulkan14.maintenance5 == VK_TRUE &&
        features.descriptor_heap.descriptorHeap == VK_TRUE &&
        features.address_commands.deviceAddressCommands == VK_TRUE &&
        features.untyped_pointers.shaderUntypedPointers == VK_TRUE &&
        features.unified_image_layouts.unifiedImageLayouts == VK_TRUE;
    if (!required_features)
        return Error::unsupported;

    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queues.data());
    const auto queue = std::ranges::find_if(queues, [](const VkQueueFamilyProperties& value) {
        constexpr auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        return value.queueCount != 0 && (value.queueFlags & required) == required;
    });
    if (queue == queues.end())
        return Error::unsupported;

    Candidate result;
    result.physical_device = physical_device;
    result.queue_family = static_cast<std::uint32_t>(std::distance(queues.begin(), queue));
    result.properties = properties;

    result.heap_properties.pNext = &result.vulkan13_properties;
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result.heap_properties,
    };
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    result.heap_properties.pNext = nullptr;
    if (result.vulkan13_properties.maxBufferSize < allocation_heap_size)
        return Error::unsupported;
    output = result;
    return Error::none;
}

Error create_shader_module(VkDevice device,
                           VkShaderModule& output,
                           std::span<const std::uint32_t> words) noexcept
{
    output = VK_NULL_HANDLE;
    assert(!words.empty() && "SPIR-V shader bytecode is empty");
    if (words.empty())
        return Error::invalid_shader;
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size_bytes(),
        .pCode = words.data(),
    };
    return error_from_vk(vkCreateShaderModule(device, &info, nullptr, &output));
}

} // namespace

Error Device::create(Device& output, const DeviceDesc& desc) noexcept
{
    static_assert(sizeof(void*) == 8, "clean_gfx requires a 64-bit host ABI");
    assert(!output && "Device::create output must be empty");
    assert(desc.application_name && "DeviceDesc::application_name must not be null");
    if (!desc.application_name)
        return Error::driver_error;

    std::uint32_t loader_version = VK_API_VERSION_1_0;
    auto error = error_from_vk(vkEnumerateInstanceVersion(&loader_version));
    if (error != Error::none)
        return error;
    if (loader_version < VK_API_VERSION_1_4)
        return Error::unsupported;

    auto state = std::make_unique<detail::DeviceState>();
#if !defined(NDEBUG)
    std::vector<VkExtensionProperties> instance_extensions;
    error = enumerate_instance_extensions(instance_extensions);
    if (error != Error::none)
        return error;
    std::vector<VkLayerProperties> layers;
    error = enumerate_instance_layers(layers);
    if (error != Error::none)
        return error;
    const bool debug_utils_available =
        has_name(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validation_available =
        has_name(layers, "VK_LAYER_KHRONOS_validation");
#endif

    std::vector<const char*> enabled_instance_extensions;
    std::vector<const char*> enabled_layers;
#if !defined(NDEBUG)
    if (debug_utils_available)
        enabled_instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation_available)
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    const VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = desc.application_name,
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "clean_gfx",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = static_cast<std::uint32_t>(enabled_layers.size()),
        .ppEnabledLayerNames = enabled_layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_instance_extensions.size()),
        .ppEnabledExtensionNames = enabled_instance_extensions.data(),
    };
    error = error_from_vk(vkCreateInstance(&instance_info, nullptr, &state->instance));
    if (error != Error::none)
        return error;

#if !defined(NDEBUG)
    if (debug_utils_available)
    {
        const auto create_debug = load_instance_proc<PFN_vkCreateDebugUtilsMessengerEXT>(
            state->instance, "vkCreateDebugUtilsMessengerEXT");
        state->destroy_debug_messenger =
            load_instance_proc<PFN_vkDestroyDebugUtilsMessengerEXT>(
                state->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!create_debug || !state->destroy_debug_messenger)
            return Error::driver_error;
        const VkDebugUtilsMessengerCreateInfoEXT debug_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
            .pUserData = nullptr,
        };
        error = error_from_vk(
            create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger));
        if (error != Error::none)
            return error;
    }
#endif

    std::vector<VkPhysicalDevice> physical_devices;
    for (;;)
    {
        std::uint32_t physical_device_count = 0;
        error = error_from_vk(
            vkEnumeratePhysicalDevices(state->instance, &physical_device_count, nullptr));
        if (error != Error::none)
            return error;
        physical_devices.resize(physical_device_count);
        const auto result = vkEnumeratePhysicalDevices(
            state->instance, &physical_device_count, physical_devices.data());
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        physical_devices.resize(physical_device_count);
        break;
    }

    Candidate selected{};
    bool has_selected = false;
    for (const auto physical_device : physical_devices)
    {
        Candidate candidate{};
        error = inspect_candidate(physical_device, candidate);
        if (error == Error::unsupported)
            continue;
        if (error != Error::none)
            return error;
        if (!has_selected || candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            selected = candidate;
            has_selected = true;
        }
        if (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            break;
    }
    if (!has_selected)
        return Error::unsupported;

    state->physical_device = selected.physical_device;
    state->queue_family = selected.queue_family;
    state->physical_properties = selected.properties;
    state->heap_properties = selected.heap_properties;
    state->vulkan13_properties = selected.vulkan13_properties;
    state->vulkan13_properties.pNext = nullptr;
    vkGetPhysicalDeviceMemoryProperties(state->physical_device, &state->memory_properties);

    QueriedFeatures enabled_features;
    vkGetPhysicalDeviceFeatures2(state->physical_device, &enabled_features.core);
    enabled_features.core.features = {};
    enabled_features.core.features.shaderInt16 = VK_TRUE;
    enabled_features.core.features.fragmentStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled_features.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enabled_features.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled_features.core.features.multiDrawIndirect = VK_TRUE;
    enabled_features.core.features.drawIndirectFirstInstance = VK_TRUE;
    enabled_features.vulkan11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &enabled_features.vulkan12,
        .storageBuffer16BitAccess = VK_TRUE,
        .uniformAndStorageBuffer16BitAccess = VK_FALSE,
        .storagePushConstant16 = VK_TRUE,
        .shaderDrawParameters = VK_TRUE,
    };
    enabled_features.vulkan12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &enabled_features.vulkan13,
        .shaderFloat16 = VK_TRUE,
        .scalarBlockLayout = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    enabled_features.vulkan13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &enabled_features.vulkan14,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    enabled_features.vulkan14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = &enabled_features.descriptor_heap,
        .maintenance5 = VK_TRUE,
    };
    enabled_features.descriptor_heap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
        .pNext = &enabled_features.address_commands,
        .descriptorHeap = VK_TRUE,
        .descriptorHeapCaptureReplay = VK_FALSE,
    };
    enabled_features.address_commands = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR,
        .pNext = &enabled_features.untyped_pointers,
        .deviceAddressCommands = VK_TRUE,
    };
    enabled_features.untyped_pointers = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
        .pNext = &enabled_features.unified_image_layouts,
        .shaderUntypedPointers = VK_TRUE,
    };
    enabled_features.unified_image_layouts = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
        .pNext = nullptr,
        .unifiedImageLayouts = VK_TRUE,
        .unifiedImageLayoutsVideo = VK_FALSE,
    };

    constexpr float queue_priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = state->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    constexpr std::array enabled_device_extensions{
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
    };
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_features.core,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_device_extensions.size()),
        .ppEnabledExtensionNames = enabled_device_extensions.data(),
        .pEnabledFeatures = nullptr,
    };
    error = error_from_vk(
        vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device));
    if (error != Error::none)
        return error;
    vkGetDeviceQueue(state->device, state->queue_family, 0, &state->queue);

    const VkSemaphoreTypeCreateInfo timeline_type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
        .flags = 0,
    };
    error = error_from_vk(
        vkCreateSemaphore(state->device, &semaphore_info, nullptr, &state->timeline));
    if (error != Error::none)
        return error;

    for (auto& frame : state->frames)
    {
        error = state->create_frame_commands(frame);
        if (error != Error::none)
            return error;
        frame.command_list_storage = ::operator new(sizeof(CommandList::Impl));
    }

    state->fn.write_sampler_descriptors =
        load_device_proc<PFN_vkWriteSamplerDescriptorsEXT>(
            state->device, "vkWriteSamplerDescriptorsEXT");
    state->fn.write_resource_descriptors =
        load_device_proc<PFN_vkWriteResourceDescriptorsEXT>(
            state->device, "vkWriteResourceDescriptorsEXT");
    state->fn.cmd_bind_sampler_heap = load_device_proc<PFN_vkCmdBindSamplerHeapEXT>(
        state->device, "vkCmdBindSamplerHeapEXT");
    state->fn.cmd_bind_resource_heap = load_device_proc<PFN_vkCmdBindResourceHeapEXT>(
        state->device, "vkCmdBindResourceHeapEXT");
    state->fn.cmd_push_data = load_device_proc<PFN_vkCmdPushDataEXT>(
        state->device, "vkCmdPushDataEXT");
    state->fn.cmd_bind_index_buffer = load_device_proc<PFN_vkCmdBindIndexBuffer3KHR>(
        state->device, "vkCmdBindIndexBuffer3KHR");
    state->fn.cmd_draw_indirect = load_device_proc<PFN_vkCmdDrawIndirect2KHR>(
        state->device, "vkCmdDrawIndirect2KHR");
    state->fn.cmd_draw_indexed_indirect =
        load_device_proc<PFN_vkCmdDrawIndexedIndirect2KHR>(
            state->device, "vkCmdDrawIndexedIndirect2KHR");
    state->fn.cmd_dispatch_indirect = load_device_proc<PFN_vkCmdDispatchIndirect2KHR>(
        state->device, "vkCmdDispatchIndirect2KHR");
    state->fn.cmd_copy_memory = load_device_proc<PFN_vkCmdCopyMemoryKHR>(
        state->device, "vkCmdCopyMemoryKHR");
    state->fn.cmd_copy_memory_to_image = load_device_proc<PFN_vkCmdCopyMemoryToImageKHR>(
        state->device, "vkCmdCopyMemoryToImageKHR");
    state->fn.cmd_copy_image_to_memory = load_device_proc<PFN_vkCmdCopyImageToMemoryKHR>(
        state->device, "vkCmdCopyImageToMemoryKHR");
    if (!state->fn.write_sampler_descriptors || !state->fn.write_resource_descriptors ||
        !state->fn.cmd_bind_sampler_heap || !state->fn.cmd_bind_resource_heap ||
        !state->fn.cmd_push_data || !state->fn.cmd_bind_index_buffer ||
        !state->fn.cmd_draw_indirect || !state->fn.cmd_draw_indexed_indirect ||
        !state->fn.cmd_dispatch_indirect || !state->fn.cmd_copy_memory ||
        !state->fn.cmd_copy_memory_to_image || !state->fn.cmd_copy_image_to_memory)
    {
        return Error::driver_error;
    }

    state->caps = {
        .device_name = state->physical_properties.deviceName,
        .api_version = state->physical_properties.apiVersion,
        .max_push_data_size = state->heap_properties.maxPushDataSize,
        .image_descriptor_size = state->heap_properties.imageDescriptorSize,
        .sampler_descriptor_size = state->heap_properties.samplerDescriptorSize,
    };
    output = Device{std::move(state)};
    return Error::none;
}

GpuAllocation Device::gpu_malloc(std::uint64_t byte_count,
                                  MemoryType memory,
                                  std::uint64_t alignment) const noexcept
{
    assert(impl_ && "gpu_malloc called on an empty device");
    if (!impl_)
        return {};
    return impl_->allocate_gpu(byte_count, memory, alignment);
}

GpuAllocation Device::gpu_malloc_resource_heap(
    std::uint64_t byte_count) const noexcept
{
    assert(impl_ && "gpu_malloc_resource_heap called on an empty device");
    if (!impl_)
        return {};
    return impl_->allocate_descriptor_heap(
        byte_count, detail::DescriptorHeapType::resource);
}

GpuAllocation Device::gpu_malloc_sampler_heap(
    std::uint64_t byte_count) const noexcept
{
    assert(impl_ && "gpu_malloc_sampler_heap called on an empty device");
    if (!impl_)
        return {};
    return impl_->allocate_descriptor_heap(
        byte_count, detail::DescriptorHeapType::sampler);
}

void Device::gpu_free(GpuAllocation allocation) const noexcept
{
    if (!allocation.gpu_ptr && !allocation.cpu_ptr && allocation.size == 0)
        return;
    assert(impl_ && "gpu_free called on an empty device");
    if (!impl_)
        return;
    impl_->release_allocation(allocation);
}

Error Device::create_texture(Texture& output, const TextureDesc& desc) const noexcept
{
    assert(!output && "create_texture output must be empty");
    assert(impl_ && "create_texture called on an empty device");
    if (!impl_)
        return Error::driver_error;
    const bool commands_recording = std::ranges::any_of(
        impl_->frames, [](const detail::FrameContext& frame) { return frame.active; });
    assert(!commands_recording &&
           "create_texture is not allowed while a command list is recording");
    if (commands_recording)
        return Error::driver_error;
    assert(desc.width != 0 && desc.height != 0 && desc.depth != 0 &&
           desc.mip_levels != 0 && "texture dimensions and mip count must be non-zero");
    const auto maximum_mip_levels = static_cast<std::uint32_t>(
        std::bit_width(std::max({desc.width, desc.height, desc.depth})));
    assert(desc.mip_levels <= maximum_mip_levels &&
           "texture mip count exceeds the maximum for its dimensions");
    const bool depth_format = desc.format == Format::d32_float;
    assert((!depth_format || !has_flag(desc.usage, TextureUsage::color_attachment)) &&
           "a depth format cannot be used as a color attachment");
    assert((depth_format || !has_flag(desc.usage, TextureUsage::depth_attachment)) &&
           "a color format cannot be used as a depth attachment");
    const bool attachment = has_flag(desc.usage, TextureUsage::color_attachment) ||
                            has_flag(desc.usage, TextureUsage::depth_attachment);
    assert((!attachment || (desc.depth == 1 && desc.mip_levels == 1)) &&
           "render attachments must be 2D single-mip textures");
    assert(desc.usage != TextureUsage::none && "texture must have at least one usage");
    constexpr auto known_usage_bits =
        static_cast<std::uint32_t>(TextureUsage::sampled) |
        static_cast<std::uint32_t>(TextureUsage::storage) |
        static_cast<std::uint32_t>(TextureUsage::color_attachment) |
        static_cast<std::uint32_t>(TextureUsage::depth_attachment) |
        static_cast<std::uint32_t>(TextureUsage::transfer_source) |
        static_cast<std::uint32_t>(TextureUsage::transfer_destination);
    const bool valid_usage_bits =
        (static_cast<std::uint32_t>(desc.usage) & ~known_usage_bits) == 0;
    assert(valid_usage_bits && "texture usage mask contains unknown bits");
    if (!impl_ || desc.width == 0 || desc.height == 0 || desc.depth == 0 ||
        desc.mip_levels == 0 || desc.mip_levels > maximum_mip_levels ||
        (depth_format && has_flag(desc.usage, TextureUsage::color_attachment)) ||
        (!depth_format && has_flag(desc.usage, TextureUsage::depth_attachment)) ||
        (attachment && (desc.depth != 1 || desc.mip_levels != 1)) ||
        desc.usage == TextureUsage::none || !valid_usage_bits)
    {
        return Error::driver_error;
    }
    if ((has_flag(desc.usage, TextureUsage::color_attachment) ||
         has_flag(desc.usage, TextureUsage::depth_attachment)) &&
        (desc.width > impl_->physical_properties.limits.maxFramebufferWidth ||
         desc.height > impl_->physical_properties.limits.maxFramebufferHeight))
    {
        return Error::unsupported;
    }
    if (has_flag(desc.usage, TextureUsage::color_attachment) ||
        has_flag(desc.usage, TextureUsage::depth_attachment))
    {
        const auto& limits = impl_->physical_properties.limits;
        const auto viewport_width = static_cast<float>(desc.width);
        const auto viewport_height = static_cast<float>(desc.height);
        if (desc.width > limits.maxViewportDimensions[0] ||
            desc.height > limits.maxViewportDimensions[1] ||
            limits.viewportBoundsRange[0] > 0.0f ||
            viewport_width > limits.viewportBoundsRange[1] ||
            viewport_height > limits.viewportBoundsRange[1])
        {
            return Error::unsupported;
        }
    }

    auto result = std::make_unique<Texture::Impl>();
    result->state = impl_.get();
    result->desc = desc;
    result->desc.name = {};

    VkImageUsageFlags usage = 0;
    if (has_flag(desc.usage, TextureUsage::sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has_flag(desc.usage, TextureUsage::storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has_flag(desc.usage, TextureUsage::color_attachment))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::depth_attachment))
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_source))
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(desc.usage, TextureUsage::transfer_destination))
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(usage != 0);

    const auto image_type = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    const auto format = to_vk(desc.format);
    const auto format_features = optimal_format_features(impl_->physical_device, desc.format);
    const auto required_features = required_format_features(desc.usage);
    if ((format_features & required_features) != required_features)
    {
        return Error::unsupported;
    }

    VkImageFormatProperties image_properties{};
    const auto format_result = vkGetPhysicalDeviceImageFormatProperties(
        impl_->physical_device,
        format,
        image_type,
        VK_IMAGE_TILING_OPTIMAL,
        usage,
        0,
        &image_properties);
    if (format_result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        return Error::unsupported;
    auto error = error_from_vk(format_result);
    if (error != Error::none)
        return error;
    if (desc.width > image_properties.maxExtent.width ||
        desc.height > image_properties.maxExtent.height ||
        desc.depth > image_properties.maxExtent.depth ||
        desc.mip_levels > image_properties.maxMipLevels ||
        (image_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0 ||
        image_resource_byte_size(desc) > image_properties.maxResourceSize)
    {
        return Error::unsupported;
    }

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = image_type,
        .format = format,
        .extent = {desc.width, desc.height, desc.depth},
        .mipLevels = desc.mip_levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    error = error_from_vk(vkCreateImage(impl_->device, &image_info, nullptr, &result->image));
    if (error != Error::none)
        return error;

    VkMemoryDedicatedRequirements dedicated_requirements{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        .pNext = nullptr,
    };
    VkMemoryRequirements2 requirements2{
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dedicated_requirements,
    };
    const VkImageMemoryRequirementsInfo2 requirements_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = nullptr,
        .image = result->image,
    };
    vkGetImageMemoryRequirements2(impl_->device, &requirements_info, &requirements2);
    const auto& requirements = requirements2.memoryRequirements;
    const bool padded_fits = requirements.size <= allocation_heap_size &&
                             requirements.alignment != 0 &&
                             requirements.alignment - 1 <=
                                 allocation_heap_size - requirements.size;
    const bool dedicated = dedicated_requirements.requiresDedicatedAllocation ||
                           !padded_fits;
    std::uint32_t memory_type = 0;
    if (!impl_->find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            0,
            dedicated ? requirements.size : allocation_heap_size,
            memory_type,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        return Error::unsupported;
    }

    if (dedicated)
    {
        const VkMemoryDedicatedAllocateInfo dedicated_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = nullptr,
            .image = result->image,
            .buffer = VK_NULL_HANDLE,
        };
        const VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated_info,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memory_type,
        };
        error = impl_->allocate_memory(allocate_info, result->dedicated_memory);
        if (error != Error::none)
            return error;
        error = error_from_vk(vkBindImageMemory(
            impl_->device, result->image, result->dedicated_memory, 0));
        if (error != Error::none)
            return error;
    }
    else
    {
        const auto padded_size = static_cast<std::uint32_t>(
            requirements.size + requirements.alignment - 1);
        const auto try_suballocate = [&](detail::ImageHeap& heap) {
            if (heap.memory_type != memory_type)
                return false;
            const auto token = heap.offsets.allocate(padded_size);
            if (token.offset == OffsetAllocator::Allocation::NO_SPACE)
                return false;
            const auto memory_offset = align_up(
                static_cast<VkDeviceSize>(token.offset), requirements.alignment);
            const bool valid_range = memory_offset <= allocation_heap_size &&
                                     requirements.size <=
                                         allocation_heap_size - memory_offset;
            assert(valid_range && "image suballocation range is invalid");
            if (!valid_range)
            {
                heap.offsets.free(token);
                return false;
            }
            result->heap = &heap;
            result->heap_allocation = token;
            result->owns_heap_allocation = true;
            error = error_from_vk(vkBindImageMemory(
                impl_->device, result->image, heap.memory, memory_offset));
            return true;
        };

        bool allocated = false;
        for (const auto& heap : impl_->image_heaps)
        {
            if (try_suballocate(*heap))
            {
                allocated = true;
                break;
            }
        }
        if (!allocated)
        {
            auto heap = std::make_unique<detail::ImageHeap>(impl_.get(), memory_type);
            const VkMemoryAllocateInfo allocate_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = nullptr,
                .allocationSize = allocation_heap_size,
                .memoryTypeIndex = memory_type,
            };
            error = impl_->allocate_memory(allocate_info, heap->memory);
            if (error != Error::none)
                return error;
            auto* heap_pointer = heap.get();
            impl_->image_heaps.push_back(std::move(heap));
            allocated = try_suballocate(*heap_pointer);
            assert(allocated && "a new image heap could not satisfy its first allocation");
            if (!allocated)
                return Error::driver_error;
        }
        if (error != Error::none)
            return error;
    }

    const bool depth = desc.format == Format::d32_float;
    const auto aspect_mask = static_cast<VkImageAspectFlags>(
        depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);

    if (attachment)
    {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = result->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = to_vk(desc.format),
            .components = {},
            .subresourceRange = {
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        error = error_from_vk(
            vkCreateImageView(impl_->device, &view_info, nullptr, &result->view));
        if (error != Error::none)
            return error;
    }

    result->initialization = {
        .image = result->image,
        .aspect_mask = aspect_mask,
        .mip_levels = desc.mip_levels,
        .initialized = false,
        .owner = &impl_->pending_texture_initializations,
    };
    impl_->pending_texture_initializations.push_back(&result->initialization);
    output = Texture{std::move(result)};
    return Error::none;
}

Error Device::write_texture_descriptor(void* cpu_destination,
                                       const Texture& texture,
                                       TextureDescriptorType type,
                                       const TextureViewDesc& view) const noexcept
{
    const bool valid = impl_ && texture.impl_ && cpu_destination &&
                       texture.impl_->state == impl_.get();
    assert(valid &&
           "write_texture_descriptor requires a destination and texture from this device");
    if (!valid)
        return Error::driver_error;

    const auto required_usage = type == TextureDescriptorType::sampled
        ? TextureUsage::sampled
        : TextureUsage::storage;
    const bool valid_type = type == TextureDescriptorType::sampled ||
                            type == TextureDescriptorType::storage;
    const bool valid_usage = valid_type &&
                             has_flag(texture.impl_->desc.usage, required_usage);
    const bool valid_base_mip = view.base_mip < texture.impl_->desc.mip_levels;
    const auto mip_count = valid_base_mip
        ? (view.mip_count == 0
               ? texture.impl_->desc.mip_levels - view.base_mip
               : view.mip_count)
        : 0;
    const bool valid_mips = valid_base_mip && mip_count != 0 &&
                            mip_count <= texture.impl_->desc.mip_levels - view.base_mip;
    assert(valid_usage && "texture was not created for this descriptor type");
    assert(valid_mips && "texture descriptor mip range is invalid");
    if (!valid_usage || !valid_mips)
        return Error::driver_error;

    const bool depth = texture.impl_->desc.format == Format::d32_float;
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = texture.impl_->image,
        .viewType = texture.impl_->desc.depth > 1
            ? VK_IMAGE_VIEW_TYPE_3D
            : VK_IMAGE_VIEW_TYPE_2D,
        .format = to_vk(texture.impl_->desc.format),
        .components = {},
        .subresourceRange = {
            .aspectMask = static_cast<VkImageAspectFlags>(
                depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
            .baseMipLevel = view.base_mip,
            .levelCount = mip_count,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const VkImageDescriptorInfoEXT image_descriptor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
        .pNext = nullptr,
        .pView = &view_info,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkResourceDescriptorDataEXT data{};
    data.pImage = &image_descriptor;
    const VkResourceDescriptorInfoEXT descriptor_info{
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .pNext = nullptr,
        .type = type == TextureDescriptorType::sampled
            ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = data,
    };
    const VkHostAddressRangeEXT destination{
        .address = cpu_destination,
        .size = static_cast<std::size_t>(impl_->heap_properties.imageDescriptorSize),
    };
    return error_from_vk(impl_->fn.write_resource_descriptors(
        impl_->device, 1, &descriptor_info, &destination));
}

Error Device::write_sampler_descriptor(void* cpu_destination,
                                       const SamplerDesc& desc) const noexcept
{
    const bool valid = impl_ && cpu_destination;
    assert(valid && "write_sampler_descriptor requires a valid device and destination");
    if (!valid)
        return Error::driver_error;
    const VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = to_vk(desc.mag_filter),
        .minFilter = to_vk(desc.min_filter),
        .mipmapMode = desc.min_filter == Filter::nearest
            ? VK_SAMPLER_MIPMAP_MODE_NEAREST
            : VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = to_vk(desc.address_u),
        .addressModeV = to_vk(desc.address_v),
        .addressModeW = to_vk(desc.address_w),
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    const VkHostAddressRangeEXT destination{
        .address = cpu_destination,
        .size = static_cast<std::size_t>(impl_->heap_properties.samplerDescriptorSize),
    };
    return error_from_vk(impl_->fn.write_sampler_descriptors(
        impl_->device, 1, &sampler_info, &destination));
}

Error Device::create_graphics_pipeline(Pipeline& output,
                                       const GraphicsPipelineDesc& desc) const noexcept
{
    assert(!output && "create_graphics_pipeline output must be empty");
    assert(impl_ && "create_graphics_pipeline called on an empty device");
    assert(desc.color_format != Format::d32_float &&
           "graphics pipeline color format must not be a depth format");
    assert((!desc.depth_enabled || desc.depth_format == Format::d32_float) &&
           "graphics pipeline depth format must be d32_float");
    assert((!desc.depth_write || desc.depth_enabled) &&
           "depth_write requires depth_enabled");
    if (!impl_ || desc.color_format == Format::d32_float ||
        (desc.depth_enabled && desc.depth_format != Format::d32_float) ||
        (desc.depth_write && !desc.depth_enabled))
    {
        return Error::driver_error;
    }
    if ((optimal_format_features(impl_->physical_device, desc.color_format) &
         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) == 0)
    {
        return Error::unsupported;
    }
    if (desc.depth_enabled &&
        (optimal_format_features(impl_->physical_device, desc.depth_format) &
         VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
    {
        return Error::unsupported;
    }

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    auto error = create_shader_module(impl_->device, vertex_module, desc.vertex_spirv);
    if (error != Error::none)
        return error;
    error = create_shader_module(impl_->device, fragment_module, desc.fragment_spirv);
    if (error != Error::none)
    {
        vkDestroyShaderModule(impl_->device, vertex_module, nullptr);
        return error;
    }

        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertex_module,
                .pName = "vertexMain",
                .pSpecializationInfo = nullptr,
            },
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragment_module,
                .pName = "fragmentMain",
                .pSpecializationInfo = nullptr,
            },
        };
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = 0,
            .pVertexBindingDescriptions = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions = nullptr,
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = to_vk(desc.topology),
            .primitiveRestartEnable = VK_FALSE,
        };
        const VkPipelineViewportStateCreateInfo viewport_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr,
        };
        const VkPipelineRasterizationStateCreateInfo rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = to_vk(desc.cull),
            .frontFace = desc.cull == CullMode::counter_clockwise
                             ? VK_FRONT_FACE_CLOCKWISE
                             : VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f,
            .lineWidth = 1.0f,
        };
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
        const VkPipelineDepthStencilStateCreateInfo depth_stencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = desc.depth_enabled ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
        };
        const VkPipelineColorBlendAttachmentState color_attachment{
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo color_blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &color_attachment,
            .blendConstants = {},
        };
        constexpr std::array dynamic_states{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        const VkPipelineDynamicStateCreateInfo dynamic_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const auto color_format = to_vk(desc.color_format);
        const auto depth_format = desc.depth_enabled ? to_vk(desc.depth_format) : VK_FORMAT_UNDEFINED;
        const VkPipelineRenderingCreateInfo rendering_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .pNext = nullptr,
            .viewMask = 0,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &color_format,
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
        };
        const VkPipelineCreateFlags2CreateInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = &rendering_info,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        const VkGraphicsPipelineCreateInfo pipeline_info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &flags_info,
            .flags = 0,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pTessellationState = nullptr,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_stencil,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic_state,
            .layout = VK_NULL_HANDLE,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        auto result = std::make_unique<Pipeline::Impl>();
        result->state = impl_.get();
        result->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        result->color_format = desc.color_format;
        result->depth_format = desc.depth_format;
        result->depth_enabled = desc.depth_enabled;
        error = error_from_vk(vkCreateGraphicsPipelines(
            impl_->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result->pipeline));

        vkDestroyShaderModule(impl_->device, fragment_module, nullptr);
        vkDestroyShaderModule(impl_->device, vertex_module, nullptr);
        if (error != Error::none)
            return error;
        output = Pipeline{std::move(result)};
        return Error::none;
}

Error Device::create_compute_pipeline(Pipeline& output,
                                      const ComputePipelineDesc& desc) const noexcept
{
    assert(!output && "create_compute_pipeline output must be empty");
    assert(impl_ && "create_compute_pipeline called on an empty device");
    if (!impl_)
        return Error::driver_error;

    VkShaderModule module = VK_NULL_HANDLE;
    auto error = create_shader_module(impl_->device, module, desc.compute_spirv);
    if (error != Error::none)
    {
        return error;
    }
        const VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "computeMain",
            .pSpecializationInfo = nullptr,
        };
        const VkPipelineCreateFlags2CreateInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        const VkComputePipelineCreateInfo pipeline_info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = &flags_info,
            .flags = 0,
            .stage = stage,
            .layout = VK_NULL_HANDLE,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        auto result = std::make_unique<Pipeline::Impl>();
        result->state = impl_.get();
        result->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        error = error_from_vk(vkCreateComputePipelines(
            impl_->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result->pipeline));
        vkDestroyShaderModule(impl_->device, module, nullptr);
        if (error != Error::none)
            return error;
        output = Pipeline{std::move(result)};
        return Error::none;
}

Error Device::begin_commands(CommandList& output) const noexcept
{
    assert(!output && "begin_commands output must be empty");
    assert(impl_ && "begin_commands called on an empty device");
    if (!impl_)
        return Error::driver_error;

    const bool already_recording = std::ranges::any_of(
        impl_->frames, [](const detail::FrameContext& frame) { return frame.active; });
    assert(!already_recording && "clean_gfx supports one recording command list at a time");
    if (already_recording)
        return Error::driver_error;
    auto& frame = impl_->frames[impl_->next_frame % impl_->frames.size()];
    if (frame.last_signal != 0)
    {
        const VkSemaphoreWaitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = 0,
            .semaphoreCount = 1,
            .pSemaphores = &impl_->timeline,
            .pValues = &frame.last_signal,
        };
        const auto error = error_from_vk(vkWaitSemaphores(
            impl_->device,
            &wait_info,
            std::numeric_limits<std::uint64_t>::max()));
        if (error != Error::none)
            return error;
        frame.last_signal = 0;
    }

    if (!frame.command_pool)
    {
        const auto error = impl_->create_frame_commands(frame);
        if (error != Error::none)
            return error;
    }
    auto error = impl_->reset_frame_commands(frame);
    if (error != Error::none)
        return error;
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    error = error_from_vk(vkBeginCommandBuffer(frame.command_buffer, &begin_info));
    if (error != Error::none)
        return error;

    assert(frame.command_list_storage && "frame command-list storage is missing");
    auto* result = ::new (frame.command_list_storage) CommandList::Impl();
    result->state = impl_.get();
    result->frame = &frame;
    result->command_buffer = frame.command_buffer;
    result->pending_texture_initializations =
        std::move(impl_->pending_texture_initializations);
    if (!result->pending_texture_initializations.empty())
    {
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.reserve(result->pending_texture_initializations.size());
        for (auto* initialization : result->pending_texture_initializations)
        {
            assert(initialization && !initialization->initialized);
            assert(initialization->owner == &impl_->pending_texture_initializations);
            initialization->owner = &result->pending_texture_initializations;
            barriers.push_back({
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = 0,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                                 VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = initialization->image,
                .subresourceRange = {
                    .aspectMask = initialization->aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = initialization->mip_levels,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            });
        }
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        };
        vkCmdPipelineBarrier2(result->command_buffer, &dependency);
    }
    frame.active = true;
    output = CommandList{result};
    return Error::none;
}

Error Device::submit(CommandList&& commands) const noexcept
{
    assert(impl_ && commands.impl_ &&
           "submit received an empty device or command list");
    assert((!impl_ || !commands.impl_ || commands.impl_->state == impl_.get()) &&
           "command list belongs to a different device");
    if (!impl_ || !commands.impl_ || commands.impl_->state != impl_.get())
        return Error::driver_error;

    CommandList owned_handle = std::move(commands);
    auto* owned = owned_handle.impl_;
    if (owned->recording)
    {
        assert(!owned->rendering && "cannot submit while a rendering scope is active");
        if (owned->rendering)
            return Error::driver_error;
        const auto error = error_from_vk(vkEndCommandBuffer(owned->command_buffer));
        if (error != Error::none)
            return error;
        owned->recording = false;
    }

    assert(owned->frame && owned->frame->active);
    if (!owned->frame || !owned->frame->active)
        return Error::driver_error;
    assert(impl_->next_signal != 0 && "timeline submission counter overflow");
    if (impl_->next_signal == 0)
        return Error::driver_error;
    const auto signal_value = impl_->next_signal;
    const VkCommandBufferSubmitInfo command_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = owned->command_buffer,
        .deviceMask = 1,
    };
    const VkSemaphoreSubmitInfo signal_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = impl_->timeline,
        .value = signal_value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = 0,
        .pWaitSemaphoreInfos = nullptr,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal_info,
    };
    const auto error = error_from_vk(
        vkQueueSubmit2(impl_->queue, 1, &submit_info, VK_NULL_HANDLE));
    if (error != Error::none)
        return error;

    for (auto* initialization : owned->pending_texture_initializations)
    {
        assert(initialization && !initialization->initialized);
        assert(initialization->owner == &owned->pending_texture_initializations);
        initialization->initialized = true;
        initialization->owner = nullptr;
    }
    owned->pending_texture_initializations.clear();
    owned->frame->last_signal = signal_value;
    owned->frame->active = false;
    owned->frame = nullptr;
    impl_->last_submitted_signal = signal_value;
    ++impl_->next_signal;
    ++impl_->next_frame;
    return Error::none;
}

Error Device::submit_and_wait(CommandList&& commands) const noexcept
{
    const auto error = submit(std::move(commands));
    if (error != Error::none)
        return error;
    const auto value = impl_->last_submitted_signal;
    const VkSemaphoreWaitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &impl_->timeline,
        .pValues = &value,
    };
    const auto wait_error = error_from_vk(vkWaitSemaphores(
        impl_->device,
        &wait_info,
        std::numeric_limits<std::uint64_t>::max()));
    if (wait_error == Error::none)
    {
        for (auto& frame : impl_->frames)
        {
            if (frame.last_signal != 0 && frame.last_signal <= value)
            {
                assert(!frame.active);
                frame.last_signal = 0;
                const auto reset_error = impl_->reset_frame_commands(frame);
                if (reset_error != Error::none)
                    return reset_error;
            }
        }
    }
    return wait_error;
}

Error Device::wait_idle() const noexcept
{
    assert(impl_ && "wait_idle called on an empty device");
    if (!impl_)
        return Error::driver_error;
    const bool commands_recording = std::ranges::any_of(
        impl_->frames, [](const detail::FrameContext& frame) { return frame.active; });
    assert(!commands_recording &&
           "wait_idle is not allowed while a command list is recording");
    if (commands_recording)
        return Error::driver_error;
    const auto error = error_from_vk(vkDeviceWaitIdle(impl_->device));
    if (error == Error::none)
    {
        for (auto& frame : impl_->frames)
        {
            frame.last_signal = 0;
            const auto reset_error = impl_->reset_frame_commands(frame);
            if (reset_error != Error::none)
                return reset_error;
        }
    }
    return error;
}

const DeviceCaps& Device::caps() const noexcept
{
    assert(impl_ && "caps called on an empty device");
    return impl_->caps;
}

void CommandList::bind_pipeline(const Pipeline& pipeline) noexcept
{
    const bool valid = impl_ && impl_->recording && pipeline.impl_;
    assert(valid && "bind_pipeline received an empty command list or pipeline");
    if (!valid)
        return;
    const bool same_device = pipeline.impl_->state == impl_->state;
    assert(same_device && "pipeline belongs to a different device");
    if (!same_device)
        return;
    vkCmdBindPipeline(
        impl_->command_buffer, pipeline.impl_->bind_point, pipeline.impl_->pipeline);
    if (pipeline.impl_->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
        impl_->bound_graphics = pipeline.impl_.get();
    else
        impl_->bound_compute = pipeline.impl_.get();
}

CommandList::AddressRange CommandList::validate_range(GpuRange range) noexcept
{
    const bool valid = impl_ && impl_->recording;
    assert(valid && "address commands require a recording command list");
    if (!valid)
        return {};
    const bool nonempty = range.gpu_ptr && range.size != 0;
    assert(nonempty && "GPU range must have a non-null pointer and non-zero size");
    if (!nonempty)
        return {};
    const auto address = static_cast<VkDeviceAddress>(
        reinterpret_cast<std::uintptr_t>(range.gpu_ptr));
    const bool address_fits =
        address <= std::numeric_limits<VkDeviceAddress>::max() - range.size;
    assert(address_fits && "GPU range address overflow");
    if (!address_fits)
        return {};
    return {
        .address = address,
        .size = range.size,
    };
}

void CommandList::validate_texture(Texture& texture) noexcept
{
    const bool valid = texture.impl_ && texture.impl_->state == impl_->state;
    assert(valid && "texture is empty or belongs to a different device");
    if (!valid)
        return;
}

bool CommandList::require_graphics_pipeline() const noexcept
{
    assert(impl_->bound_graphics && "draw requires a bound graphics pipeline");
    if (!impl_->bound_graphics)
        return false;
    const bool formats_match =
        impl_->bound_graphics->color_format == impl_->rendering_color_format &&
        impl_->bound_graphics->depth_enabled == impl_->rendering_has_depth &&
        (!impl_->rendering_has_depth ||
         impl_->bound_graphics->depth_format == impl_->rendering_depth_format);
    assert(formats_match &&
           "graphics pipeline formats do not match the active rendering attachments");
    return formats_match;
}

bool CommandList::require_compute_pipeline() const noexcept
{
    assert(impl_->bound_compute && "dispatch requires a bound compute pipeline");
    return impl_->bound_compute != nullptr;
}

bool CommandList::emit_root_data(const void* data, std::size_t size) noexcept
{
    const bool has_data = data != nullptr;
    const bool valid = has_data == (size != 0) &&
                       (!has_data || ((size & 3u) == 0 &&
                                      size <= impl_->state->heap_properties.maxPushDataSize));
    assert(valid &&
           "draw/dispatch root data must be null with zero size or non-null, "
           "four-byte sized, and fit "
           "VkPhysicalDeviceDescriptorHeapPropertiesEXT::maxPushDataSize");
    if (!valid)
        return false;
    if (!has_data)
        return true;
    const VkPushDataInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .pNext = nullptr,
        .offset = 0,
        .data = {
            .address = data,
            .size = size,
        },
    };
    impl_->state->fn.cmd_push_data(impl_->command_buffer, &info);
    return true;
}

void CommandList::set_resource_heap(GpuRange heap) noexcept
{
    const bool valid = impl_ && impl_->recording;
    assert(valid && "set_resource_heap requires a recording command list");
    if (!valid)
        return;
    const auto& properties = impl_->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    if (!make_heap_bind_info(
            heap,
            properties.resourceHeapAlignment,
            std::max(properties.imageDescriptorAlignment,
                     properties.bufferDescriptorAlignment),
            properties.minResourceHeapReservedRange,
            properties.maxResourceHeapSize,
            bind_info))
    {
        return;
    }
    impl_->state->fn.cmd_bind_resource_heap(impl_->command_buffer, &bind_info);
}

void CommandList::set_sampler_heap(GpuRange heap) noexcept
{
    const bool valid = impl_ && impl_->recording;
    assert(valid && "set_sampler_heap requires a recording command list");
    if (!valid)
        return;
    const auto& properties = impl_->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    if (!make_heap_bind_info(heap,
                             properties.samplerHeapAlignment,
                             properties.samplerDescriptorAlignment,
                             properties.minSamplerHeapReservedRange,
                             properties.maxSamplerHeapSize,
                             bind_info))
    {
        return;
    }
    impl_->state->fn.cmd_bind_sampler_heap(impl_->command_buffer, &bind_info);
}

void CommandList::begin_rendering(Texture& color,
                                  float4 clear_color,
                                  bool clear,
                                  Texture* depth,
                                  float clear_depth) noexcept
{
    const bool valid = impl_ && impl_->recording && color.impl_;
    assert(valid && "begin_rendering received an empty object");
    if (!valid)
        return;
    assert(!impl_->rendering && "a rendering scope is already active");
    const bool valid_color = color.impl_->state == impl_->state &&
                             has_flag(color.impl_->desc.usage,
                                      TextureUsage::color_attachment);
    assert(valid_color &&
           "render target must belong to the device and support color attachments");
    if (impl_->rendering || !valid_color)
        return;
    if (depth)
    {
        const bool valid_depth = depth->impl_ && depth->impl_->state == impl_->state;
        assert(valid_depth && "depth target is empty or belongs to a different device");
        if (!valid_depth)
            return;
        const bool depth_usage =
            has_flag(depth->impl_->desc.usage, TextureUsage::depth_attachment) &&
            depth->impl_->desc.format == Format::d32_float;
        assert(depth_usage &&
               "depth target must be a D32 texture created for depth-attachment use");
        const bool dimensions_match =
            depth->impl_->desc.width == color.impl_->desc.width &&
            depth->impl_->desc.height == color.impl_->desc.height;
        assert(dimensions_match && "color and depth target dimensions differ");
        if (!depth_usage || !dimensions_match)
            return;
    }
    const bool valid_clear_depth = !depth || !clear ||
                                   (clear_depth >= 0.0f && clear_depth <= 1.0f);
    assert(valid_clear_depth && "clear depth must be in the [0, 1] range");
    if (!valid_clear_depth)
        return;

    initialize_texture(*impl_,
                       *color.impl_,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (depth)
    {
        initialize_texture(*impl_,
                           *depth->impl_,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }

    validate_texture(color);
    if (depth)
        validate_texture(*depth);

    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = color.impl_->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{clear_color.x, clear_color.y, clear_color.z, clear_color.w}}},
    };
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depth ? depth->impl_->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {clear_depth, 0}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {{0, 0}, {color.impl_->desc.width, color.impl_->desc.height}},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = depth ? &depth_attachment : nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(impl_->command_buffer, &rendering_info);

    const VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(color.impl_->desc.width),
        .height = static_cast<float>(color.impl_->desc.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor{{0, 0}, {color.impl_->desc.width, color.impl_->desc.height}};
    vkCmdSetViewport(impl_->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(impl_->command_buffer, 0, 1, &scissor);
    impl_->rendering = true;
    impl_->rendering_color_format = color.impl_->desc.format;
    impl_->rendering_has_depth = depth != nullptr;
    if (depth)
        impl_->rendering_depth_format = depth->impl_->desc.format;
}

void CommandList::end_rendering() noexcept
{
    const bool valid = impl_ && impl_->recording && impl_->rendering;
    assert(valid && "end_rendering requires an active rendering scope");
    if (!valid)
        return;
    vkCmdEndRendering(impl_->command_buffer);
    impl_->rendering = false;
    impl_->rendering_has_depth = false;
}

void CommandList::draw_impl(const void* root,
                            std::size_t root_size,
                            std::uint32_t vertex_count,
                            std::uint32_t instance_count,
                            std::uint32_t first_vertex,
                            std::uint32_t first_instance) noexcept
{
    const bool valid = impl_ && impl_->recording && impl_->rendering;
    assert(valid && "draw requires an active rendering scope");
    if (!valid)
        return;
    if (!require_graphics_pipeline())
        return;
    if (!emit_root_data(root, root_size))
        return;
    vkCmdDraw(impl_->command_buffer,
              vertex_count,
              instance_count,
              first_vertex,
              first_instance);
}

void CommandList::draw_indexed_impl(const void* root,
                                    std::size_t root_size,
                                    GpuRange indices,
                                    IndexType type,
                                    std::uint32_t index_count,
                                    std::uint32_t instance_count,
                                    std::uint32_t first_index,
                                    std::int32_t vertex_offset,
                                    std::uint32_t first_instance) noexcept
{
    const bool valid = impl_ && impl_->recording && impl_->rendering;
    assert(valid && "draw_indexed requires an active rendering scope");
    if (!valid)
        return;
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    if (!valid_type)
        return;
    if (!require_graphics_pipeline())
        return;
    const auto range = validate_range(indices);
    const auto alignment = type == IndexType::uint16 ? 2u : 4u;
    assert(range.address % alignment == 0 &&
           "index address range is empty or misaligned");
    const auto index_size = static_cast<std::uint64_t>(alignment);
    const bool range_fits =
        first_index <= range.size / index_size &&
        index_count <= (range.size - static_cast<std::uint64_t>(first_index) * index_size) /
                           index_size;
    assert(range_fits && "indexed draw exceeds the bound index address range");
    if (range.address % alignment != 0 || !range_fits)
        return;

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {range.address, range.size},
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    impl_->state->fn.cmd_bind_index_buffer(impl_->command_buffer, &bind_info);
    if (!emit_root_data(root, root_size))
        return;
    vkCmdDrawIndexed(impl_->command_buffer,
                     index_count,
                     instance_count,
                     first_index,
                     vertex_offset,
                     first_instance);
}

void CommandList::draw_indirect_impl(const void* root,
                                     std::size_t root_size,
                                     GpuRange arguments,
                                     std::uint32_t draw_count,
                                     std::uint32_t stride) noexcept
{
    const bool valid = impl_ && impl_->recording && impl_->rendering;
    assert(valid && "draw_indirect requires an active rendering scope");
    if (!valid)
        return;
    if (!require_graphics_pipeline())
        return;
    if (stride == 0)
        stride = sizeof(VkDrawIndirectCommand);
    const auto required_size = draw_count == 0
                                   ? 0ull
                                   : static_cast<std::uint64_t>(draw_count - 1) * stride +
                                         sizeof(VkDrawIndirectCommand);
    const bool valid_arguments =
        draw_count != 0 && stride >= sizeof(VkDrawIndirectCommand) &&
        (stride & 3u) == 0 &&
        draw_count <= impl_->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "indirect draw range, count, or stride is invalid");
    if (!valid_arguments)
        return;
    const auto range = validate_range(arguments);
    const bool range_fits = (range.address & 3u) == 0 &&
                            range.size >= required_size && stride <= range.size;
    assert(range_fits && "indirect draw range, count, or stride is invalid");
    if (!range_fits)
        return;
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {range.address, range.size, stride},
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    if (!emit_root_data(root, root_size))
        return;
    impl_->state->fn.cmd_draw_indirect(impl_->command_buffer, &info);
}

void CommandList::draw_indexed_indirect_impl(const void* root,
                                             std::size_t root_size,
                                             GpuRange indices,
                                             IndexType type,
                                             GpuRange arguments,
                                             std::uint32_t draw_count,
                                             std::uint32_t stride) noexcept
{
    const bool valid = impl_ && impl_->recording && impl_->rendering;
    assert(valid && "draw_indexed_indirect requires an active rendering scope");
    if (!valid)
        return;
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    if (!valid_type)
        return;
    if (!require_graphics_pipeline())
        return;
    const auto index_range = validate_range(indices);
    const auto index_alignment = type == IndexType::uint16 ? 2u : 4u;
    const bool valid_index_range = index_range.address % index_alignment == 0;
    assert(valid_index_range && "index address range is empty or misaligned");
    if (!valid_index_range)
        return;
    if (stride == 0)
        stride = sizeof(VkDrawIndexedIndirectCommand);
    const auto required_size = draw_count == 0
                                   ? 0ull
                                   : static_cast<std::uint64_t>(draw_count - 1) * stride +
                                         sizeof(VkDrawIndexedIndirectCommand);
    const bool valid_arguments =
        draw_count != 0 && stride >= sizeof(VkDrawIndexedIndirectCommand) &&
        (stride & 3u) == 0 &&
        draw_count <= impl_->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments &&
           "indexed indirect draw range, count, or stride is invalid");
    if (!valid_arguments)
        return;
    const auto argument_range = validate_range(arguments);
    const bool range_fits = (argument_range.address & 3u) == 0 &&
                            argument_range.size >= required_size &&
                            stride <= argument_range.size;
    assert(range_fits && "indexed indirect draw range, count, or stride is invalid");
    if (!range_fits)
        return;

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {index_range.address, index_range.size},
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    impl_->state->fn.cmd_bind_index_buffer(impl_->command_buffer, &bind_info);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {argument_range.address, argument_range.size, stride},
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    if (!emit_root_data(root, root_size))
        return;
    impl_->state->fn.cmd_draw_indexed_indirect(impl_->command_buffer, &info);
}

void CommandList::dispatch_impl(const void* root,
                                std::size_t root_size,
                                std::uint32_t x,
                                std::uint32_t y,
                                std::uint32_t z) noexcept
{
    const bool valid = impl_ && impl_->recording && !impl_->rendering;
    assert(valid && "dispatch requires a recording command list outside rendering");
    if (!valid)
        return;
    if (!require_compute_pipeline())
        return;
    const auto& limits = impl_->state->physical_properties.limits.maxComputeWorkGroupCount;
    const bool count_fits = x <= limits[0] && y <= limits[1] && z <= limits[2];
    assert(count_fits &&
           "dispatch group count exceeds VkPhysicalDeviceLimits::maxComputeWorkGroupCount");
    if (!count_fits)
        return;
    if (!emit_root_data(root, root_size))
        return;
    vkCmdDispatch(impl_->command_buffer, x, y, z);
}

void CommandList::dispatch_indirect_impl(const void* root,
                                         std::size_t root_size,
                                         GpuRange arguments) noexcept
{
    const bool valid = impl_ && impl_->recording && !impl_->rendering;
    assert(valid &&
           "dispatch_indirect requires a recording command list outside rendering");
    if (!valid)
        return;
    if (!require_compute_pipeline())
        return;
    const auto range = validate_range(arguments);
    const bool range_fits = (range.address & 3u) == 0 &&
                            range.size >= sizeof(VkDispatchIndirectCommand);
    assert(range_fits && "indirect dispatch range is too small or misaligned");
    if (!range_fits)
        return;
    const VkDispatchIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {range.address, range.size},
        .addressFlags = address_flags,
    };
    if (!emit_root_data(root, root_size))
        return;
    impl_->state->fn.cmd_dispatch_indirect(impl_->command_buffer, &info);
}

void CommandList::copy_memory(GpuRange source, GpuRange destination) noexcept
{
    const bool valid = impl_ && impl_->recording && !impl_->rendering;
    assert(valid && "copy_memory requires a recording command list outside rendering");
    if (!valid)
        return;
    const auto source_range = validate_range(source);
    const auto destination_range = validate_range(destination);
    const bool destination_fits = destination_range.size >= source_range.size;
    assert(destination_fits &&
           "copy_memory destination range is smaller than its source range");
    const bool overlaps =
        source_range.address < destination_range.address + source_range.size &&
        destination_range.address < source_range.address + source_range.size;
    assert(!overlaps && "copy_memory source and destination ranges overlap");
    if (!destination_fits || overlaps)
        return;

    const VkDeviceMemoryCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .pNext = nullptr,
        .srcRange = {source_range.address, source_range.size},
        .srcFlags = address_flags,
        .dstRange = {destination_range.address, destination_range.size},
        .dstFlags = address_flags,
    };
    const VkCopyDeviceMemoryInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        .pNext = nullptr,
        .regionCount = 1,
        .pRegions = &region,
    };
    impl_->state->fn.cmd_copy_memory(impl_->command_buffer, &info);
}

void CommandList::copy_memory_to_texture(GpuRange source, Texture& destination) noexcept
{
    const bool valid = impl_ && impl_->recording && !impl_->rendering && destination.impl_;
    assert(valid &&
           "copy_memory_to_texture received an empty object or active rendering scope");
    if (!valid)
        return;
    const bool valid_texture = destination.impl_->state == impl_->state &&
                               has_flag(destination.impl_->desc.usage,
                                        TextureUsage::transfer_destination);
    assert(valid_texture &&
           "texture must belong to the device and support transfer destination use");
    if (!valid_texture)
        return;
    validate_texture(destination);
    const auto texel_size = bytes_per_texel(destination.impl_->desc.format);
    const auto required_size = base_level_byte_size(destination.impl_->desc);
    const auto source_range = validate_range(source);
    const bool range_fits = source_range.address % texel_size == 0 &&
                            source_range.size >= required_size;
    assert(range_fits &&
           "source range is empty, misaligned, or too small for the texture base level");
    if (!range_fits)
        return;

    initialize_texture(*impl_,
                       *destination.impl_,
                       VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const bool depth = destination.impl_->desc.format == Format::d32_float;
    const VkDeviceMemoryImageCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .pNext = nullptr,
        .addressRange = {source_range.address, source_range.size},
        .addressFlags = address_flags,
        .addressRowLength = 0,
        .addressImageHeight = 0,
        .imageSubresource = {
            .aspectMask = static_cast<VkImageAspectFlags>(
                depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset = {0, 0, 0},
        .imageExtent = {destination.impl_->desc.width,
                        destination.impl_->desc.height,
                        destination.impl_->desc.depth},
    };
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .pNext = nullptr,
        .image = destination.impl_->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    impl_->state->fn.cmd_copy_memory_to_image(impl_->command_buffer, &info);
}

void CommandList::copy_texture_to_memory(Texture& source, GpuRange destination) noexcept
{
    const bool valid = impl_ && impl_->recording && !impl_->rendering && source.impl_;
    assert(valid &&
           "copy_texture_to_memory received an empty object or active rendering scope");
    if (!valid)
        return;
    const bool valid_texture = source.impl_->state == impl_->state &&
                               has_flag(source.impl_->desc.usage,
                                        TextureUsage::transfer_source);
    assert(valid_texture &&
           "texture must belong to the device and support transfer source use");
    if (!valid_texture)
        return;
    validate_texture(source);
    const auto texel_size = bytes_per_texel(source.impl_->desc.format);
    const auto required_size = base_level_byte_size(source.impl_->desc);
    const auto destination_range = validate_range(destination);
    const bool range_fits = destination_range.address % texel_size == 0 &&
                            destination_range.size >= required_size;
    assert(range_fits &&
           "destination range is empty, misaligned, or too small for the texture base level");
    if (!range_fits)
        return;

    initialize_texture(*impl_,
                       *source.impl_,
                       VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);

    const bool depth = source.impl_->desc.format == Format::d32_float;
    const VkDeviceMemoryImageCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .pNext = nullptr,
        .addressRange = {destination_range.address, destination_range.size},
        .addressFlags = address_flags,
        .addressRowLength = 0,
        .addressImageHeight = 0,
        .imageSubresource = {
            .aspectMask = static_cast<VkImageAspectFlags>(
                depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset = {0, 0, 0},
        .imageExtent = {source.impl_->desc.width,
                        source.impl_->desc.height,
                        source.impl_->desc.depth},
    };
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .pNext = nullptr,
        .image = source.impl_->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    impl_->state->fn.cmd_copy_image_to_memory(impl_->command_buffer, &info);
}

void CommandList::barrier(Stage before,
                          Access before_access,
                          Stage after,
                          Access after_access) noexcept
{
    const bool valid = impl_ && impl_->recording && !impl_->rendering;
    assert(valid && "barrier requires a recording command list outside rendering");
    if (!valid)
        return;
    validate_stage_access(before, before_access);
    validate_stage_access(after, after_access);
    const VkMemoryBarrier2 memory_barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = to_vk(before),
        .srcAccessMask = to_vk(before_access),
        .dstStageMask = to_vk(after),
        .dstAccessMask = to_vk(after_access),
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory_barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(impl_->command_buffer, &dependency);
}

Error CommandList::finish() noexcept
{
    const bool valid = impl_ && impl_->recording;
    assert(valid && "finish requires a recording command list");
    if (!valid)
        return Error::driver_error;
    assert(!impl_->rendering && "finish called while a rendering scope is active");
    if (impl_->rendering)
        return Error::driver_error;
    const auto error = error_from_vk(vkEndCommandBuffer(impl_->command_buffer));
    if (error != Error::none)
    {
        std::destroy_at(std::exchange(impl_, nullptr));
        return error;
    }
    impl_->recording = false;
    return Error::none;
}

Texture::Texture() noexcept = default;
Texture::~Texture() = default;
Texture::Texture(Texture&&) noexcept = default;
Texture& Texture::operator=(Texture&&) noexcept = default;
Texture::Texture(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::uint32_t Texture::width() const noexcept
{
    assert(impl_ && "width called on an empty texture");
    return impl_ ? impl_->desc.width : 0;
}

std::uint32_t Texture::height() const noexcept
{
    assert(impl_ && "height called on an empty texture");
    return impl_ ? impl_->desc.height : 0;
}
Texture::operator bool() const noexcept { return impl_ != nullptr; }

Pipeline::Pipeline() noexcept = default;
Pipeline::~Pipeline() = default;
Pipeline::Pipeline(Pipeline&&) noexcept = default;
Pipeline& Pipeline::operator=(Pipeline&&) noexcept = default;
Pipeline::Pipeline(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Pipeline::operator bool() const noexcept { return impl_ != nullptr; }

CommandList::CommandList() noexcept = default;
CommandList::~CommandList()
{
    if (impl_)
        std::destroy_at(std::exchange(impl_, nullptr));
}
CommandList::CommandList(CommandList&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr))
{}
CommandList& CommandList::operator=(CommandList&& other) noexcept
{
    if (this == &other)
        return *this;
    if (impl_)
        std::destroy_at(impl_);
    impl_ = std::exchange(other.impl_, nullptr);
    return *this;
}
CommandList::CommandList(Impl* impl) noexcept : impl_(impl) {}
CommandList::operator bool() const noexcept { return impl_ != nullptr; }

Device::Device() noexcept = default;
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;
Device::Device(std::unique_ptr<detail::DeviceState> impl) noexcept : impl_(std::move(impl)) {}
Device::operator bool() const noexcept { return impl_ != nullptr; }

} // namespace gfx
