#include <clean_gfx/clean_gfx.hpp>

#include <offsetAllocator.hpp>
#include <vulkan/vulkan.h>

#include <bit>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace gfx
{
namespace
{

constexpr std::uint32_t allocation_heap_size = 256u * 1024u * 1024u;
constexpr std::uint32_t max_device_extensions = 512;
constexpr std::uint32_t max_instance_extensions = 256;
constexpr std::uint32_t max_instance_layers = 64;
constexpr std::uint32_t max_physical_devices = 32;
constexpr std::uint32_t max_queue_families = 64;
constexpr std::uint32_t image_barrier_batch_size = 64;
constexpr std::uint32_t frame_count = 2;

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
    case VK_ERROR_TOO_MANY_OBJECTS: return Error::out_of_device_memory;
    default: return Error::driver_error;
    }
}

[[noreturn]] void abort_vk_failure(VkResult result) noexcept
{
    (void)result;
    assert(result == VK_SUCCESS && "unexpected Vulkan failure");
    std::abort();
}

void require_vk(VkResult result) noexcept
{
    if (result != VK_SUCCESS)
        abort_vk_failure(result);
}

[[nodiscard]] Error allocation_error_from_vk(VkResult result) noexcept
{
    if (result == VK_SUCCESS)
        return Error::none;
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_TOO_MANY_OBJECTS)
        return Error::out_of_device_memory;
    abort_vk_failure(result);
}

void require_error(Error error) noexcept
{
    if (error != Error::none)
    {
        assert(false && "unexpected graphics API failure");
        std::abort();
    }
}

[[noreturn]] void abort_api_failure() noexcept
{
    assert(false && "invalid graphics API call");
    std::abort();
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
    if (alignment == 0 || value > std::numeric_limits<T>::max() - (alignment - 1))
        abort_api_failure();
    return ((value + alignment - 1) / alignment) * alignment;
}

template<typename T>
constexpr bool has_flag(T value, T flag)
{
    using U = std::underlying_type_t<T>;
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

bool has_name(const Span<const VkExtensionProperties>& values, const char* name)
{
    for (std::size_t index = 0; index < values.size; ++index)
    {
        if (std::strcmp(values.data[index].extensionName, name) == 0)
            return true;
    }
    return false;
}

#if !defined(NDEBUG)
bool has_name(const Span<const VkLayerProperties>& values, const char* name)
{
    for (std::size_t index = 0; index < values.size; ++index)
    {
        if (std::strcmp(values.data[index].layerName, name) == 0)
            return true;
    }
    return false;
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
    std::abort();
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
    std::abort();
}

std::uint64_t base_level_byte_size(Format format,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint32_t depth)
{
    std::uint64_t size = bytes_per_texel(format);
    bool fits = width == 0 || size <= std::numeric_limits<std::uint64_t>::max() / width;
    assert(fits && "texture byte size overflow");
    if (!fits)
        abort_api_failure();
    size *= width;
    fits = height == 0 || size <= std::numeric_limits<std::uint64_t>::max() / height;
    assert(fits && "texture byte size overflow");
    if (!fits)
        abort_api_failure();
    size *= height;
    fits = depth == 0 || size <= std::numeric_limits<std::uint64_t>::max() / depth;
    assert(fits && "texture byte size overflow");
    if (!fits)
        abort_api_failure();
    size *= depth;
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
        std::uint64_t level_size = base_level_byte_size(desc.format, width, height, depth);
        const bool fits = total <= std::numeric_limits<std::uint64_t>::max() - level_size;
        assert(fits && "texture mip-chain byte size overflow");
        if (!fits)
            abort_api_failure();
        total += level_size;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        depth = depth > 1 ? depth / 2 : 1;
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
    std::abort();
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
    std::abort();
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
    std::abort();
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
    std::abort();
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
        abort_api_failure();
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
        abort_api_failure();
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
    if (stages == Stage::none)
        abort_api_failure();
    if (stages == Stage::all)
        return;

    const auto has_shader_stage = has_flag(stages, Stage::vertex) ||
                                  has_flag(stages, Stage::fragment) ||
                                  has_flag(stages, Stage::compute);
    const auto transfer_stage = has_flag(stages, Stage::transfer);
    const bool compatible =
        (!has_flag(accesses, Access::transfer_read) || transfer_stage) &&
        (!has_flag(accesses, Access::transfer_write) || transfer_stage) &&
        (!has_flag(accesses, Access::shader_read) || has_shader_stage) &&
        (!has_flag(accesses, Access::shader_write) || has_shader_stage) &&
        (!has_flag(accesses, Access::descriptor_read) || has_shader_stage) &&
        (!has_flag(accesses, Access::color_read) || has_flag(stages, Stage::color_output)) &&
        (!has_flag(accesses, Access::color_write) || has_flag(stages, Stage::color_output)) &&
        (!has_flag(accesses, Access::depth_read) || has_flag(stages, Stage::depth_tests)) &&
        (!has_flag(accesses, Access::depth_write) || has_flag(stages, Stage::depth_tests)) &&
        (!has_flag(accesses, Access::indirect_read) || has_flag(stages, Stage::indirect)) &&
        (!has_flag(accesses, Access::index_read) || has_flag(stages, Stage::index_input)) &&
        (!has_flag(accesses, Access::host_read) || has_flag(stages, Stage::host)) &&
        (!has_flag(accesses, Access::host_write) || has_flag(stages, Stage::host));
    assert(compatible && "access is incompatible with the stage mask");
    if (!compatible)
        abort_api_failure();
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
        std::abort();
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
struct FrameContext;

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
    TextureInitialization* previous = nullptr;
    TextureInitialization* next = nullptr;
    struct TextureInitializationList* owner = nullptr;
};

struct TextureInitializationList
{
    TextureInitialization* first = nullptr;
    TextureInitialization* last = nullptr;
};

void append_texture_initialization(TextureInitializationList& list,
                                   TextureInitialization& initialization) noexcept
{
    assert(!initialization.owner && !initialization.previous && !initialization.next);
    if (initialization.owner || initialization.previous || initialization.next)
        abort_api_failure();
    initialization.owner = &list;
    initialization.previous = list.last;
    if (list.last)
        list.last->next = &initialization;
    else
        list.first = &initialization;
    list.last = &initialization;
}

void remove_texture_initialization(TextureInitialization& initialization) noexcept
{
    TextureInitializationList* list = initialization.owner;
    assert(list);
    if (!list)
        abort_api_failure();
    if (initialization.previous)
        initialization.previous->next = initialization.next;
    else
        list->first = initialization.next;
    if (initialization.next)
        initialization.next->previous = initialization.previous;
    else
        list->last = initialization.previous;
    initialization.owner = nullptr;
    initialization.previous = nullptr;
    initialization.next = nullptr;
}

void transfer_texture_initializations(TextureInitializationList& destination,
                                      TextureInitializationList& source) noexcept
{
    assert(!destination.first && !destination.last);
    if (destination.first || destination.last)
        abort_api_failure();
    destination = source;
    source = {};
    for (TextureInitialization* current = destination.first; current; current = current->next)
        current->owner = &destination;
}

} // namespace detail

struct CommandList
{
    Device* state = nullptr;
    detail::FrameContext* frame = nullptr;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    bool recording = false;
    bool rendering = false;
    Format rendering_color_format = Format::rgba8_unorm;
    Format rendering_depth_format = Format::d32_float;
    bool rendering_has_depth = false;
    const Pipeline* bound_graphics = nullptr;
    const Pipeline* bound_compute = nullptr;
    detail::TextureInitializationList pending_texture_initializations;
};

namespace detail
{

struct FrameContext
{
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    CommandList commands{};
    std::uint64_t last_signal = 0;
    bool active = false;
};

} // namespace detail

struct Device
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
    detail::DeviceFunctions fn;
    DeviceCaps caps;
    mutable std::uint32_t live_memory_allocations = 0;
    detail::AllocationHeap* allocation_heaps[3]{};
    detail::DescriptorAllocation* descriptor_allocations = nullptr;
    detail::ImageHeap* image_heaps = nullptr;
    detail::TextureInitializationList pending_texture_initializations;
    detail::FrameContext frames[frame_count]{};
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t next_frame = 0;
    std::uint64_t next_signal = 1;
    std::uint64_t last_submitted_signal = 0;

    ~Device();

    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    void destroy_backing(detail::BackingBuffer& backing) const noexcept
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
            abort_api_failure();
        const auto heap_index = memory_properties.memoryTypes[info.memoryTypeIndex].heapIndex;
        assert(heap_index < memory_properties.memoryHeapCount);
        if (heap_index >= memory_properties.memoryHeapCount)
            abort_api_failure();
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
            return allocation_error_from_vk(result);
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
        bool has_best = false;
        std::uint32_t best = 0;
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
            if (!has_best || score > best_score ||
                (score == best_score && heap.size > best_heap_size))
            {
                best = i;
                has_best = true;
                best_score = score;
                best_heap_size = heap.size;
            }
        }
        if (!has_best)
            return false;
        output = best;
        return true;
    }

    [[nodiscard]] Error create_backing_buffer(
        detail::BackingBuffer& output,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred,
        VkMemoryPropertyFlags forbidden = 0) const noexcept
    {
        output = {};
        assert(size != 0);
        if (size == 0 || size > vulkan13_properties.maxBufferSize)
            abort_api_failure();
        detail::BackingBuffer result{
            .size = size,
        };
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
        auto error = allocation_error_from_vk(
            vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));
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
            abort_api_failure();
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
        error = allocation_error_from_vk(
            vkBindBufferMemory(device, result.buffer, result.memory, 0));
        if (error != Error::none)
        {
            destroy_backing(result);
            return error;
        }

        if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            error = allocation_error_from_vk(
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
            abort_api_failure();
        }
        output = result;
        return Error::none;
    }

    [[nodiscard]] GpuAllocation<> allocate_gpu(
        VkDeviceSize size,
        MemoryType memory,
        VkDeviceSize alignment) noexcept;
    [[nodiscard]] detail::AllocationHeap* create_allocation_heap(
        MemoryType memory) noexcept;
    [[nodiscard]] bool try_allocate_gpu(detail::AllocationHeap& heap,
                                        VkDeviceSize size,
                                        std::uint32_t padded_size,
                                        VkDeviceSize alignment,
                                        GpuAllocation<>& output) noexcept;
    [[nodiscard]] GpuAllocation<> allocate_descriptor_heap(
        VkDeviceSize size,
        detail::DescriptorHeapType type) noexcept;
    void release_allocation(const GpuAllocation<>& allocation) noexcept;
    [[nodiscard]] Error create_frame_commands(detail::FrameContext& frame) noexcept;
    void destroy_frame_commands(detail::FrameContext& frame) noexcept;
    void reset_frame_commands(detail::FrameContext& frame) noexcept;

};

namespace detail
{

struct ImageHeap
{
    ImageHeap(Device* owner, std::uint32_t type)
        : state(owner), memory_type(type), offsets(allocation_heap_size)
    {}

    ~ImageHeap()
    {
        if (state && memory)
            state->free_memory(memory);
    }

    Device* state = nullptr;
    ImageHeap* next = nullptr;
    std::uint32_t memory_type = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    OffsetAllocator::Allocator offsets;
};

struct AllocationRecord
{
    OffsetAllocator::Allocation offset_allocation{};
    std::uint32_t aligned_offset = 0;
    std::uint32_t requested_size = 0;
};

struct AllocationHeap
{
    AllocationHeap(Device* owner, MemoryType type)
        : state(owner), memory(type), offsets(allocation_heap_size)
    {
        allocations.reserve(1024);
    }

    ~AllocationHeap()
    {
        if (state)
            state->destroy_backing(backing);
    }

    Device* state = nullptr;
    AllocationHeap* next = nullptr;
    MemoryType memory;
    BackingBuffer backing;
    OffsetAllocator::Allocator offsets;
    // The public allocation is only {cpu, gpu, size}, so gpu_free needs this
    // persistent per-page table to recover OffsetAllocator's opaque token.
    std::vector<AllocationRecord> allocations;
};

struct DescriptorAllocation
{
    explicit DescriptorAllocation(Device* owner) : state(owner) {}

    ~DescriptorAllocation()
    {
        if (state)
            state->destroy_backing(backing);
    }

    Device* state = nullptr;
    DescriptorAllocation* next = nullptr;
    BackingBuffer backing;
    GpuAllocation<> value{};
};

} // namespace detail

Device::~Device()
{
    if (device)
        vkDeviceWaitIdle(device);
    for (auto& frame : frames)
    {
        assert(!frame.active && "device destroyed while a command list is active");
        if (frame.active)
            abort_api_failure();
        destroy_frame_commands(frame);
        frame = {};
    }
    for (detail::AllocationHeap*& pool : allocation_heaps)
    {
        while (pool)
        {
            detail::AllocationHeap* heap = pool;
            pool = heap->next;
            delete heap;
        }
    }
    while (descriptor_allocations)
    {
        detail::DescriptorAllocation* allocation = descriptor_allocations;
        descriptor_allocations = allocation->next;
        delete allocation;
    }
    while (image_heaps)
    {
        detail::ImageHeap* heap = image_heaps;
        image_heaps = heap->next;
        delete heap;
    }
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

Error Device::create_frame_commands(detail::FrameContext& frame) noexcept
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

void Device::destroy_frame_commands(detail::FrameContext& frame) noexcept
{
    if (device && frame.command_pool)
        vkDestroyCommandPool(device, frame.command_pool, nullptr);
    frame.command_pool = VK_NULL_HANDLE;
    frame.command_buffer = VK_NULL_HANDLE;
}

void Device::reset_frame_commands(detail::FrameContext& frame) noexcept
{
    if (!frame.command_pool)
    {
        assert(!frame.command_buffer);
        return;
    }
    assert(frame.command_buffer);
    require_vk(vkResetCommandPool(device, frame.command_pool, 0));
}

detail::AllocationHeap* Device::create_allocation_heap(MemoryType memory) noexcept
{
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
        abort_api_failure();
    }

    detail::AllocationHeap* heap = new detail::AllocationHeap(this, memory);
    const Error error = create_backing_buffer(
        heap->backing,
        allocation_heap_size,
        universal_buffer_usage(),
        required,
        preferred,
        forbidden);
    if (error != Error::none)
    {
        delete heap;
        return nullptr;
    }
    return heap;
}

bool Device::try_allocate_gpu(detail::AllocationHeap& heap,
                              VkDeviceSize size,
                              std::uint32_t padded_size,
                              VkDeviceSize alignment,
                              GpuAllocation<>& output) noexcept
{
    if (heap.backing.mapped)
    {
        const std::uintptr_t host_base =
            reinterpret_cast<std::uintptr_t>(heap.backing.mapped);
        if ((host_base % alignment) != (heap.backing.address % alignment))
            abort_api_failure();
    }

    const OffsetAllocator::Allocation token = heap.offsets.allocate(padded_size);
    if (token.offset == OffsetAllocator::Allocation::NO_SPACE)
        return false;

    const VkDeviceAddress raw_address = heap.backing.address + token.offset;
    const VkDeviceAddress gpu_address = align_up(raw_address, alignment);
    const VkDeviceSize offset = gpu_address - heap.backing.address;
    const bool valid_range = offset <= allocation_heap_size &&
                             size <= allocation_heap_size - offset;
    assert(valid_range && "OffsetAllocator returned an invalid padded range");
    if (!valid_range)
    {
        heap.offsets.free(token);
        abort_api_failure();
    }

    std::byte* cpu_pointer = nullptr;
    if (heap.backing.mapped)
    {
        const std::uintptr_t host_address =
            reinterpret_cast<std::uintptr_t>(heap.backing.mapped) + offset;
        assert(host_address % alignment == 0 &&
               "mapped GPU allocation does not satisfy its requested alignment");
        if (host_address % alignment != 0)
        {
            heap.offsets.free(token);
            abort_api_failure();
        }
        cpu_pointer = reinterpret_cast<std::byte*>(host_address);
    }
    assert(gpu_address % alignment == 0 &&
           "GPU allocation does not satisfy its requested alignment");

    assert(offset <= std::numeric_limits<std::uint32_t>::max() &&
           size <= std::numeric_limits<std::uint32_t>::max());
    const detail::AllocationRecord record{
        .offset_allocation = token,
        .aligned_offset = static_cast<std::uint32_t>(offset),
        .requested_size = static_cast<std::uint32_t>(size),
    };
    output = {
        .cpu = cpu_pointer,
        .gpu = reinterpret_cast<std::byte*>(
            static_cast<std::uintptr_t>(gpu_address)),
        .size = size,
    };
    heap.allocations.push_back(record);
    return true;
}

GpuAllocation<> Device::allocate_gpu(VkDeviceSize size,
                                     MemoryType memory,
                                     VkDeviceSize alignment) noexcept
{
    assert(size != 0 && "gpu_malloc byte count must be non-zero");
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0 &&
           "gpu_malloc alignment must be a non-zero power of two");
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
        abort_api_failure();

    const bool request_fits = size <= allocation_heap_size && alignment != 0 &&
                               alignment - 1 <= allocation_heap_size - size;
    assert(request_fits && "gpu_malloc request including alignment padding exceeds 256 MiB");
    if (!request_fits)
        abort_api_failure();

    const auto padded_size = static_cast<std::uint32_t>(size + alignment - 1);
    const auto pool_index = memory_pool_index(memory);
    detail::AllocationHeap*& pool = allocation_heaps[pool_index];
    for (detail::AllocationHeap* heap = pool; heap; heap = heap->next)
    {
        GpuAllocation<> allocation{};
        if (try_allocate_gpu(*heap, size, padded_size, alignment, allocation))
            return allocation;
    }

    detail::AllocationHeap* heap = create_allocation_heap(memory);
    if (!heap)
        return {};
    heap->next = pool;
    pool = heap;
    GpuAllocation<> allocation{};
    if (try_allocate_gpu(*heap, size, padded_size, alignment, allocation))
        return allocation;
    abort_api_failure();
}

GpuAllocation<> Device::allocate_descriptor_heap(
    VkDeviceSize size,
    detail::DescriptorHeapType type) noexcept
{
    assert(size != 0 && "descriptor heap byte count must be non-zero");
    if (size == 0)
        abort_api_failure();

    assert(type == detail::DescriptorHeapType::resource ||
           type == detail::DescriptorHeapType::sampler);
    const bool resource = type == detail::DescriptorHeapType::resource;
    const auto resource_alignment =
        heap_properties.imageDescriptorAlignment > heap_properties.bufferDescriptorAlignment
            ? heap_properties.imageDescriptorAlignment
            : heap_properties.bufferDescriptorAlignment;
    const auto reserved_alignment = resource
        ? resource_alignment
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
        abort_api_failure();
    }

    const auto reserved_offset = align_up(size, reserved_alignment);
    if (reserved_offset > maximum_size ||
        reserved_size > maximum_size - reserved_offset)
    {
        abort_api_failure();
    }
    const auto bind_size = reserved_offset + reserved_size;

    detail::DescriptorAllocation* allocation = new detail::DescriptorAllocation(this);
    const auto error = create_backing_buffer(
        allocation->backing,
        bind_size,
        universal_buffer_usage() | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
        cpu_visible_memory_properties,
        0);
    if (error != Error::none)
    {
        delete allocation;
        return {};
    }

    const bool valid_backing =
        allocation->backing.mapped != nullptr &&
        allocation->backing.address % heap_alignment == 0;
    assert(valid_backing && "descriptor heap backing address is invalid");
    if (!valid_backing)
        abort_api_failure();

    allocation->value = {
        .cpu = reinterpret_cast<std::byte*>(allocation->backing.mapped),
        .gpu = reinterpret_cast<std::byte*>(
            static_cast<std::uintptr_t>(allocation->backing.address)),
        .size = size,
    };
    const auto result = allocation->value;
    allocation->next = descriptor_allocations;
    descriptor_allocations = allocation;
    return result;
}

void Device::release_allocation(const GpuAllocation<>& allocation) noexcept
{
    if (!allocation.gpu)
    {
        assert(!allocation.cpu && allocation.size == 0 &&
               "gpu_free received an invalid empty allocation");
        return;
    }

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(allocation.gpu);
    for (detail::AllocationHeap* pool : allocation_heaps)
    {
        for (detail::AllocationHeap* heap = pool; heap; heap = heap->next)
        {
            const std::uintptr_t heap_begin =
                static_cast<std::uintptr_t>(heap->backing.address);
            const std::uintptr_t heap_end = heap_begin + heap->backing.size;
            if (address < heap_begin || address >= heap_end)
                continue;

            const std::uintptr_t allocation_offset = address - heap_begin;
            for (std::size_t count = heap->allocations.size(); count != 0; --count)
            {
                const std::size_t index = count - 1;
                const detail::AllocationRecord& record = heap->allocations[index];
                if (record.aligned_offset != allocation_offset)
                    continue;
                std::byte* expected_cpu = nullptr;
                if (heap->backing.mapped)
                {
                    expected_cpu = reinterpret_cast<std::byte*>(
                        reinterpret_cast<std::uintptr_t>(heap->backing.mapped) +
                        record.aligned_offset);
                }
                const bool matches = expected_cpu == allocation.cpu &&
                                     record.requested_size == allocation.size;
                assert(matches &&
                       "gpu_free allocation fields do not match the original gpu_malloc result");
                if (!matches)
                    abort_api_failure();
                heap->offsets.free(record.offset_allocation);
                if (index != heap->allocations.size() - 1)
                    heap->allocations[index] = heap->allocations.back();
                heap->allocations.pop_back();
                return;
            }
            assert(false && "gpu_free address belongs to a heap but not a live allocation");
            abort_api_failure();
        }
    }

    detail::DescriptorAllocation** link = &descriptor_allocations;
    while (*link)
    {
        detail::DescriptorAllocation* record = *link;
        if (record->value.gpu != allocation.gpu)
        {
            link = &record->next;
            continue;
        }
        const bool matches = record->value.cpu == allocation.cpu &&
                             record->value.size == allocation.size;
        assert(matches &&
               "gpu_free allocation fields do not match the original descriptor heap allocation");
        if (!matches)
            abort_api_failure();
        *link = record->next;
        delete record;
        return;
    }
    assert(false && "gpu_free requires a live allocation returned by a GPU allocator");
    abort_api_failure();
}

struct Texture
{
    Device* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory dedicated_memory = VK_NULL_HANDLE;
    detail::ImageHeap* heap = nullptr;
    OffsetAllocator::Allocation heap_allocation{};
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t mip_levels = 0;
    Format format = Format::rgba8_unorm;
    TextureUsage usage = TextureUsage::none;
    bool owns_heap_allocation = false;
    detail::TextureInitialization initialization;

    ~Texture()
    {
        if (!state)
            return;
        if (initialization.owner)
            detail::remove_texture_initialization(initialization);
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

struct Pipeline
{
    Device* state = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    Format color_format = Format::rgba8_unorm;
    Format depth_format = Format::d32_float;
    bool depth_enabled = false;

    ~Pipeline()
    {
        if (state && pipeline)
            vkDestroyPipeline(state->device, pipeline, nullptr);
    }
};

struct AddressRange
{
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

namespace
{

void record_image_barriers(VkCommandBuffer command_buffer,
                           const Span<const VkImageMemoryBarrier2>& barriers) noexcept
{
    const bool valid = command_buffer && barriers.data && barriers.size != 0 &&
                       barriers.size <= std::numeric_limits<std::uint32_t>::max();
    assert(valid && "image barrier batch is invalid");
    if (!valid)
        abort_api_failure();
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size),
        .pImageMemoryBarriers = barriers.data,
    };
    vkCmdPipelineBarrier2(command_buffer, &dependency);
}

void make_heap_bind_info(const GpuRange& heap,
                         VkDeviceSize heap_alignment,
                         VkDeviceSize reserved_alignment,
                         VkDeviceSize reserved_size,
                         VkDeviceSize maximum_size,
                         VkBindHeapInfoEXT& output) noexcept
{
    const bool nonempty = heap.gpu && heap.size != 0;
    assert(nonempty && "descriptor heap range must be non-empty");
    if (!nonempty || reserved_alignment == 0 || heap_alignment == 0 ||
        heap.size > std::numeric_limits<VkDeviceSize>::max() -
                        (reserved_alignment - 1))
    {
        abort_api_failure();
    }
    const auto address = static_cast<VkDeviceAddress>(
        reinterpret_cast<std::uintptr_t>(heap.gpu));
    const auto reserved_offset = align_up<VkDeviceSize>(heap.size, reserved_alignment);
    const bool size_fits = reserved_offset <= maximum_size &&
                           reserved_size <= maximum_size - reserved_offset;
    assert(size_fits && "descriptor heap size exceeds the implementation limit");
    if (!size_fits)
        abort_api_failure();
    const auto total_size = reserved_offset + reserved_size;
    const bool valid = address % heap_alignment == 0 &&
                       address <= std::numeric_limits<VkDeviceAddress>::max() - total_size;
    assert(valid &&
           "descriptor heap address, user size, or implementation reservation is invalid");
    if (!valid)
        abort_api_failure();
    output = {
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext = nullptr,
        .heapRange = {
            .address = address,
            .size = total_size,
        },
        .reservedRangeOffset = reserved_offset,
        .reservedRangeSize = reserved_size,
    };
}

Error enumerate_device_extensions(VkPhysicalDevice physical_device,
                                  const Span<VkExtensionProperties>& values,
                                  std::uint32_t& count) noexcept
{
    for (;;)
    {
        std::uint32_t available = 0;
        auto error = error_from_vk(
            vkEnumerateDeviceExtensionProperties(
                physical_device, nullptr, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const auto result = vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
}

#if !defined(NDEBUG)
Error enumerate_instance_extensions(const Span<VkExtensionProperties>& values,
                                    std::uint32_t& count) noexcept
{
    for (;;)
    {
        std::uint32_t available = 0;
        auto error = error_from_vk(
            vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const auto result =
            vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
}

Error enumerate_instance_layers(const Span<VkLayerProperties>& values,
                                std::uint32_t& count) noexcept
{
    for (;;)
    {
        std::uint32_t available = 0;
        auto error = error_from_vk(
            vkEnumerateInstanceLayerProperties(&available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const auto result = vkEnumerateInstanceLayerProperties(&count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
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

    VkExtensionProperties extensions[max_device_extensions]{};
    std::uint32_t extension_count = 0;
    const auto extension_error = enumerate_device_extensions(
        physical_device, {extensions, max_device_extensions}, extension_count);
    if (extension_error != Error::none)
        return extension_error;
    constexpr const char* required_extensions[]{
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
    };
    for (const char* name : required_extensions)
    {
        if (!has_name({extensions, extension_count}, name))
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

    std::uint32_t available_queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &available_queue_count, nullptr);
    if (available_queue_count > max_queue_families)
        return Error::unsupported;
    VkQueueFamilyProperties queues[max_queue_families]{};
    std::uint32_t queue_count = available_queue_count;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_count, queues);
    std::uint32_t queue_family = queue_count;
    constexpr auto required_queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (std::uint32_t index = 0; index < queue_count; ++index)
    {
        if (queues[index].queueCount != 0 &&
            (queues[index].queueFlags & required_queue_flags) == required_queue_flags)
        {
            queue_family = index;
            break;
        }
    }
    if (queue_family == queue_count)
        return Error::unsupported;

    Candidate result{
        .physical_device = physical_device,
        .queue_family = queue_family,
        .properties = properties,
    };

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

void create_shader_module(VkDevice device,
                          VkShaderModule& output,
                          const Span<const std::uint32_t>& words) noexcept
{
    output = VK_NULL_HANDLE;
    const bool valid = words.data && words.size != 0 &&
                       words.size <= std::numeric_limits<std::size_t>::max() /
                                         sizeof(std::uint32_t);
    assert(valid && "SPIR-V shader bytecode is empty or too large");
    if (!valid)
        abort_api_failure();
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = words.size * sizeof(std::uint32_t),
        .pCode = words.data,
    };
    require_vk(vkCreateShaderModule(device, &info, nullptr, &output));
}

Error enumerate_physical_devices(VkInstance instance,
                                 const Span<VkPhysicalDevice>& values,
                                 std::uint32_t& count) noexcept
{
    for (;;)
    {
        std::uint32_t available = 0;
        Error error = error_from_vk(
            vkEnumeratePhysicalDevices(instance, &available, nullptr));
        if (error != Error::none)
            return error;
        if (available > values.size)
            return Error::unsupported;
        count = available;
        const VkResult result = vkEnumeratePhysicalDevices(
            instance, &count, values.data);
        if (result == VK_INCOMPLETE)
            continue;
        error = error_from_vk(result);
        if (error != Error::none)
            return error;
        return Error::none;
    }
}

DeviceInit fail_device_creation(Device* device, Error error) noexcept
{
    delete device;
    return {
        .device = nullptr,
        .error = error,
    };
}

} // namespace

DeviceInit create_device(const DeviceDesc& desc) noexcept
{
    static_assert(sizeof(void*) == 8, "clean_gfx requires a 64-bit host ABI");
    assert(desc.application_name && "DeviceDesc::application_name must not be null");
    if (!desc.application_name)
        abort_api_failure();

    std::uint32_t loader_version = VK_API_VERSION_1_0;
    auto error = error_from_vk(vkEnumerateInstanceVersion(&loader_version));
    if (error != Error::none)
        return {
            .device = nullptr,
            .error = error,
        };
    if (loader_version < VK_API_VERSION_1_4)
        return {
            .device = nullptr,
            .error = Error::unsupported,
        };

    auto* state = new Device;
#if !defined(NDEBUG)
    VkExtensionProperties instance_extensions[max_instance_extensions]{};
    std::uint32_t instance_extension_count = 0;
    error = enumerate_instance_extensions(
        {instance_extensions, max_instance_extensions}, instance_extension_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
    VkLayerProperties layers[max_instance_layers]{};
    std::uint32_t layer_count = 0;
    error = enumerate_instance_layers(
        {layers, max_instance_layers}, layer_count);
    if (error != Error::none)
        return fail_device_creation(state, error);
    const bool debug_utils_available =
        has_name({instance_extensions, instance_extension_count},
                 VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validation_available =
        has_name({layers, layer_count}, "VK_LAYER_KHRONOS_validation");
#endif

    const char* enabled_instance_extensions[1]{};
    std::uint32_t enabled_instance_extension_count = 0;
    const char* enabled_layers[1]{};
    std::uint32_t enabled_layer_count = 0;
#if !defined(NDEBUG)
    if (debug_utils_available)
        enabled_instance_extensions[enabled_instance_extension_count++] =
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (validation_available)
        enabled_layers[enabled_layer_count++] = "VK_LAYER_KHRONOS_validation";
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
        .enabledLayerCount = enabled_layer_count,
        .ppEnabledLayerNames = enabled_layer_count ? enabled_layers : nullptr,
        .enabledExtensionCount = enabled_instance_extension_count,
        .ppEnabledExtensionNames = enabled_instance_extension_count
            ? enabled_instance_extensions
            : nullptr,
    };
    error = error_from_vk(vkCreateInstance(&instance_info, nullptr, &state->instance));
    if (error != Error::none)
        return fail_device_creation(state, error);

#if !defined(NDEBUG)
    if (debug_utils_available)
    {
        const auto create_debug = load_instance_proc<PFN_vkCreateDebugUtilsMessengerEXT>(
            state->instance, "vkCreateDebugUtilsMessengerEXT");
        state->destroy_debug_messenger =
            load_instance_proc<PFN_vkDestroyDebugUtilsMessengerEXT>(
                state->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!create_debug || !state->destroy_debug_messenger)
            return fail_device_creation(state, Error::driver_error);
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
            return fail_device_creation(state, error);
    }
#endif

    VkPhysicalDevice physical_devices[max_physical_devices]{};
    std::uint32_t physical_device_count = 0;
    error = enumerate_physical_devices(
        state->instance,
        {physical_devices, max_physical_devices},
        physical_device_count);
    if (error != Error::none)
        return fail_device_creation(state, error);

    Candidate selected{};
    bool has_selected = false;
    for (std::uint32_t index = 0; index < physical_device_count; ++index)
    {
        const VkPhysicalDevice physical_device = physical_devices[index];
        Candidate candidate{};
        error = inspect_candidate(physical_device, candidate);
        if (error == Error::unsupported)
            continue;
        if (error != Error::none)
            return fail_device_creation(state, error);
        if (!has_selected || candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            selected = candidate;
            has_selected = true;
        }
        if (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            break;
    }
    if (!has_selected)
        return fail_device_creation(state, Error::unsupported);

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
    constexpr const char* enabled_device_extensions[]{
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
        .enabledExtensionCount = static_cast<std::uint32_t>(
            sizeof(enabled_device_extensions) / sizeof(enabled_device_extensions[0])),
        .ppEnabledExtensionNames = enabled_device_extensions,
        .pEnabledFeatures = nullptr,
    };
    error = error_from_vk(
        vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device));
    if (error != Error::none)
        return fail_device_creation(state, error);
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
        return fail_device_creation(state, error);

    for (auto& frame : state->frames)
    {
        error = state->create_frame_commands(frame);
        if (error != Error::none)
            return fail_device_creation(state, error);
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
        return fail_device_creation(state, Error::driver_error);
    }

    state->caps = {
        .device_name = state->physical_properties.deviceName,
        .api_version = state->physical_properties.apiVersion,
        .max_push_data_size = state->heap_properties.maxPushDataSize,
        .image_descriptor_size = state->heap_properties.imageDescriptorSize,
        .sampler_descriptor_size = state->heap_properties.samplerDescriptorSize,
    };
    return {
        .device = state,
        .error = Error::none,
    };
}

GpuAllocation<> gpu_malloc(Device* device,
                           std::uint64_t byte_count,
                           MemoryType memory,
                           std::uint64_t alignment) noexcept
{
    assert(device && "gpu_malloc called with a null device");
    if (!device)
        std::abort();
    return device->allocate_gpu(byte_count, memory, alignment);
}

GpuAllocation<> gpu_malloc_resource_heap(Device* device,
                                         std::uint64_t byte_count) noexcept
{
    assert(device && "gpu_malloc_resource_heap called with a null device");
    if (!device)
        std::abort();
    return device->allocate_descriptor_heap(
        byte_count, detail::DescriptorHeapType::resource);
}

GpuAllocation<> gpu_malloc_sampler_heap(Device* device,
                                        std::uint64_t byte_count) noexcept
{
    assert(device && "gpu_malloc_sampler_heap called with a null device");
    if (!device)
        std::abort();
    return device->allocate_descriptor_heap(
        byte_count, detail::DescriptorHeapType::sampler);
}

void gpu_free(Device* device, const GpuAllocation<>& allocation) noexcept
{
    if (!allocation.gpu && !allocation.cpu && allocation.size == 0)
        return;
    assert(device && "gpu_free called with a null device");
    if (!device)
        std::abort();
    device->release_allocation(allocation);
}

namespace
{

bool has_active_commands(const Device& device) noexcept
{
    for (const detail::FrameContext& frame : device.frames)
    {
        if (frame.active)
            return true;
    }
    return false;
}

bool try_suballocate_image(Device& device,
                           Texture& texture,
                           detail::ImageHeap& heap,
                           std::uint32_t memory_type,
                           const VkMemoryRequirements& requirements,
                           std::uint32_t padded_size) noexcept
{
    if (heap.memory_type != memory_type)
        return false;
    const OffsetAllocator::Allocation token = heap.offsets.allocate(padded_size);
    if (token.offset == OffsetAllocator::Allocation::NO_SPACE)
        return false;
    const VkDeviceSize memory_offset = align_up(
        static_cast<VkDeviceSize>(token.offset), requirements.alignment);
    const bool valid_range = memory_offset <= allocation_heap_size &&
                             requirements.size <= allocation_heap_size - memory_offset;
    assert(valid_range && "image suballocation range is invalid");
    if (!valid_range)
    {
        heap.offsets.free(token);
        abort_api_failure();
    }
    texture.heap = &heap;
    texture.heap_allocation = token;
    texture.owns_heap_allocation = true;
    require_vk(vkBindImageMemory(device.device, texture.image, heap.memory, memory_offset));
    return true;
}

} // namespace

Texture* create_texture(Device* device, const TextureDesc& desc) noexcept
{
    assert(device && "create_texture called with a null device");
    if (!device)
        std::abort();
    const bool commands_recording = has_active_commands(*device);
    assert(!commands_recording &&
           "create_texture is not allowed while a command list is recording");
    if (commands_recording)
        std::abort();
    assert(desc.width != 0 && desc.height != 0 && desc.depth != 0 &&
           desc.mip_levels != 0 && "texture dimensions and mip count must be non-zero");
    std::uint32_t maximum_dimension = desc.width > desc.height ? desc.width : desc.height;
    if (desc.depth > maximum_dimension)
        maximum_dimension = desc.depth;
    const auto maximum_mip_levels = static_cast<std::uint32_t>(
        std::bit_width(maximum_dimension));
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
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 ||
        desc.mip_levels == 0 || desc.mip_levels > maximum_mip_levels ||
        (depth_format && has_flag(desc.usage, TextureUsage::color_attachment)) ||
        (!depth_format && has_flag(desc.usage, TextureUsage::depth_attachment)) ||
        (attachment && (desc.depth != 1 || desc.mip_levels != 1)) ||
        desc.usage == TextureUsage::none || !valid_usage_bits)
    {
        abort_api_failure();
    }
    if ((has_flag(desc.usage, TextureUsage::color_attachment) ||
         has_flag(desc.usage, TextureUsage::depth_attachment)) &&
        (desc.width > device->physical_properties.limits.maxFramebufferWidth ||
         desc.height > device->physical_properties.limits.maxFramebufferHeight))
    {
        abort_api_failure();
    }
    if (has_flag(desc.usage, TextureUsage::color_attachment) ||
        has_flag(desc.usage, TextureUsage::depth_attachment))
    {
        const auto& limits = device->physical_properties.limits;
        const auto viewport_width = static_cast<float>(desc.width);
        const auto viewport_height = static_cast<float>(desc.height);
        if (desc.width > limits.maxViewportDimensions[0] ||
            desc.height > limits.maxViewportDimensions[1] ||
            limits.viewportBoundsRange[0] > 0.0f ||
            viewport_width > limits.viewportBoundsRange[1] ||
            viewport_height > limits.viewportBoundsRange[1])
        {
            abort_api_failure();
        }
    }

    auto* result = new Texture{
        .state = device,
        .width = desc.width,
        .height = desc.height,
        .depth = desc.depth,
        .mip_levels = desc.mip_levels,
        .format = desc.format,
        .usage = desc.usage,
    };

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
    const auto format_features = optimal_format_features(device->physical_device, desc.format);
    const auto required_features = required_format_features(desc.usage);
    if ((format_features & required_features) != required_features)
    {
        abort_api_failure();
    }

    VkImageFormatProperties image_properties{};
    const auto format_result = vkGetPhysicalDeviceImageFormatProperties(
        device->physical_device,
        format,
        image_type,
        VK_IMAGE_TILING_OPTIMAL,
        usage,
        0,
        &image_properties);
    if (format_result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        abort_api_failure();
    require_vk(format_result);
    auto error = Error::none;
    if (desc.width > image_properties.maxExtent.width ||
        desc.height > image_properties.maxExtent.height ||
        desc.depth > image_properties.maxExtent.depth ||
        desc.mip_levels > image_properties.maxMipLevels ||
        (image_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0 ||
        image_resource_byte_size(desc) > image_properties.maxResourceSize)
    {
        abort_api_failure();
    }

    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = image_type,
        .format = format,
        .extent = {
            .width = desc.width,
            .height = desc.height,
            .depth = desc.depth,
        },
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
    require_vk(vkCreateImage(device->device, &image_info, nullptr, &result->image));

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
    vkGetImageMemoryRequirements2(device->device, &requirements_info, &requirements2);
    const auto& requirements = requirements2.memoryRequirements;
    const bool padded_fits = requirements.size <= allocation_heap_size &&
                             requirements.alignment != 0 &&
                             requirements.alignment - 1 <=
                                 allocation_heap_size - requirements.size;
    const bool dedicated = dedicated_requirements.requiresDedicatedAllocation ||
                           !padded_fits;
    std::uint32_t memory_type = 0;
    if (!device->find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            0,
            dedicated ? requirements.size : allocation_heap_size,
            memory_type,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        abort_api_failure();
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
        error = device->allocate_memory(allocate_info, result->dedicated_memory);
        require_error(error);
        require_vk(vkBindImageMemory(
            device->device, result->image, result->dedicated_memory, 0));
    }
    else
    {
        const auto padded_size = static_cast<std::uint32_t>(
            requirements.size + requirements.alignment - 1);
        bool allocated = false;
        for (detail::ImageHeap* heap = device->image_heaps; heap; heap = heap->next)
        {
            if (try_suballocate_image(
                    *device, *result, *heap, memory_type, requirements, padded_size))
            {
                allocated = true;
                break;
            }
        }
        if (!allocated)
        {
            detail::ImageHeap* heap = new detail::ImageHeap(device, memory_type);
            const VkMemoryAllocateInfo allocate_info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = nullptr,
                .allocationSize = allocation_heap_size,
                .memoryTypeIndex = memory_type,
            };
            error = device->allocate_memory(allocate_info, heap->memory);
            require_error(error);
            heap->next = device->image_heaps;
            device->image_heaps = heap;
            allocated = try_suballocate_image(
                *device, *result, *heap, memory_type, requirements, padded_size);
            assert(allocated && "a new image heap could not satisfy its first allocation");
            if (!allocated)
                abort_api_failure();
        }
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
        require_vk(vkCreateImageView(
            device->device, &view_info, nullptr, &result->view));
    }

    result->initialization = {
        .image = result->image,
        .aspect_mask = aspect_mask,
        .mip_levels = desc.mip_levels,
        .initialized = false,
    };
    detail::append_texture_initialization(
        device->pending_texture_initializations, result->initialization);
    return result;
}

void write_texture_descriptor(Device* device,
                              void* cpu_destination,
                              const Texture* texture,
                              TextureDescriptorType type,
                              const TextureViewDesc& view) noexcept
{
    const bool valid = device && texture && cpu_destination && texture->state == device;
    assert(valid &&
           "write_texture_descriptor requires a destination and texture from this device");
    if (!valid)
        abort_api_failure();

    const auto required_usage = type == TextureDescriptorType::sampled
        ? TextureUsage::sampled
        : TextureUsage::storage;
    const bool valid_type = type == TextureDescriptorType::sampled ||
                            type == TextureDescriptorType::storage;
    const bool valid_usage = valid_type &&
                             has_flag(texture->usage, required_usage);
    const bool valid_base_mip = view.base_mip < texture->mip_levels;
    const auto mip_count = valid_base_mip
        ? (view.mip_count == 0
               ? texture->mip_levels - view.base_mip
               : view.mip_count)
        : 0;
    const bool valid_mips = valid_base_mip && mip_count != 0 &&
                            mip_count <= texture->mip_levels - view.base_mip;
    assert(valid_usage && "texture was not created for this descriptor type");
    assert(valid_mips && "texture descriptor mip range is invalid");
    if (!valid_usage || !valid_mips)
        abort_api_failure();

    const bool depth = texture->format == Format::d32_float;
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = texture->image,
        .viewType = texture->depth > 1
            ? VK_IMAGE_VIEW_TYPE_3D
            : VK_IMAGE_VIEW_TYPE_2D,
        .format = to_vk(texture->format),
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
    const VkResourceDescriptorDataEXT data{
        .pImage = &image_descriptor,
    };
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
        .size = static_cast<std::size_t>(device->heap_properties.imageDescriptorSize),
    };
    require_vk(device->fn.write_resource_descriptors(
        device->device, 1, &descriptor_info, &destination));
}

void write_sampler_descriptor(Device* device,
                              void* cpu_destination,
                              const SamplerDesc& desc) noexcept
{
    const bool valid = device && cpu_destination;
    assert(valid && "write_sampler_descriptor requires a valid device and destination");
    if (!valid)
        abort_api_failure();
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
        .size = static_cast<std::size_t>(device->heap_properties.samplerDescriptorSize),
    };
    require_vk(device->fn.write_sampler_descriptors(
        device->device, 1, &sampler_info, &destination));
}

Pipeline* create_graphics_pipeline(Device* device, const GraphicsPipelineDesc& desc) noexcept
{
    assert(device && "create_graphics_pipeline called with a null device");
    assert(desc.color_format != Format::d32_float &&
           "graphics pipeline color format must not be a depth format");
    assert((!desc.depth_enabled || desc.depth_format == Format::d32_float) &&
           "graphics pipeline depth format must be d32_float");
    assert((!desc.depth_write || desc.depth_enabled) && "depth_write requires depth_enabled");
    if (!device || desc.color_format == Format::d32_float ||
        (desc.depth_enabled && desc.depth_format != Format::d32_float) ||
        (desc.depth_write && !desc.depth_enabled))
    {
        abort_api_failure();
    }
    if ((optimal_format_features(device->physical_device, desc.color_format) &
         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) == 0)
    {
        abort_api_failure();
    }
    if (desc.depth_enabled && (optimal_format_features(device->physical_device, desc.depth_format) &
                               VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
    {
        abort_api_failure();
    }

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    create_shader_module(device->device, vertex_module, desc.vertex_spirv);
    create_shader_module(device->device, fragment_module, desc.fragment_spirv);

    const VkPipelineShaderStageCreateInfo stages[]{
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
        .frontFace = desc.cull == CullMode::counter_clockwise ? VK_FRONT_FACE_CLOCKWISE
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
    constexpr VkDynamicState dynamic_states[]{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<std::uint32_t>(
            sizeof(dynamic_states) / sizeof(dynamic_states[0])),
        .pDynamicStates = dynamic_states,
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
        .stageCount = static_cast<std::uint32_t>(sizeof(stages) / sizeof(stages[0])),
        .pStages = stages,
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
    auto* result = new Pipeline{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .color_format = desc.color_format,
        .depth_format = desc.depth_format,
        .depth_enabled = desc.depth_enabled,
    };
    const auto pipeline_result = vkCreateGraphicsPipelines(
        device->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result->pipeline);

    vkDestroyShaderModule(device->device, fragment_module, nullptr);
    vkDestroyShaderModule(device->device, vertex_module, nullptr);
    require_vk(pipeline_result);
    return result;
}

Pipeline* create_compute_pipeline(Device* device, const ComputePipelineDesc& desc) noexcept
{
    assert(device && "create_compute_pipeline called with a null device");
    if (!device)
        abort_api_failure();

    VkShaderModule module = VK_NULL_HANDLE;
    create_shader_module(device->device, module, desc.compute_spirv);
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
    auto* result = new Pipeline{
        .state = device,
        .bind_point = VK_PIPELINE_BIND_POINT_COMPUTE,
    };
    const auto pipeline_result = vkCreateComputePipelines(
        device->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result->pipeline);
    vkDestroyShaderModule(device->device, module, nullptr);
    require_vk(pipeline_result);
    return result;
}

CommandList* begin_commands(Device* device) noexcept
{
    assert(device && "begin_commands called with a null device");
    if (!device)
        abort_api_failure();

    const bool already_recording = has_active_commands(*device);
    assert(!already_recording && "clean_gfx supports one recording command list at a time");
    if (already_recording)
        abort_api_failure();
    auto& frame = device->frames[device->next_frame % frame_count];
    if (frame.last_signal != 0)
    {
        const VkSemaphoreWaitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = 0,
            .semaphoreCount = 1,
            .pSemaphores = &device->timeline,
            .pValues = &frame.last_signal,
        };
        require_vk(vkWaitSemaphores(
            device->device,
            &wait_info,
            std::numeric_limits<std::uint64_t>::max()));
        frame.last_signal = 0;
    }

    assert(frame.command_pool && frame.command_buffer);
    if (!frame.command_pool || !frame.command_buffer)
        abort_api_failure();
    device->reset_frame_commands(frame);
    const VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    require_vk(vkBeginCommandBuffer(frame.command_buffer, &begin_info));

    CommandList* result = &frame.commands;
    assert(!result->state && !result->frame && !result->recording);
    if (result->state || result->frame || result->recording)
        abort_api_failure();
    *result = {
        .state = device,
        .frame = &frame,
        .command_buffer = frame.command_buffer,
        .recording = true,
    };
    detail::transfer_texture_initializations(
        result->pending_texture_initializations,
        device->pending_texture_initializations);
    if (result->pending_texture_initializations.first)
    {
        VkImageMemoryBarrier2 barriers[image_barrier_batch_size]{};
        std::uint32_t barrier_count = 0;
        for (detail::TextureInitialization* initialization =
                 result->pending_texture_initializations.first;
             initialization;
             initialization = initialization->next)
        {
            assert(initialization && !initialization->initialized);
            assert(initialization->owner == &result->pending_texture_initializations);
            if (!initialization || initialization->initialized ||
                initialization->owner != &result->pending_texture_initializations)
            {
                abort_api_failure();
            }
            barriers[barrier_count++] = {
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
            };
            if (barrier_count == image_barrier_batch_size)
            {
                record_image_barriers(
                    result->command_buffer, {barriers, barrier_count});
                barrier_count = 0;
            }
        }
        if (barrier_count != 0)
            record_image_barriers(
                result->command_buffer, {barriers, barrier_count});
    }
    frame.active = true;
    return result;
}

void submit(Device* device, CommandList* commands) noexcept
{
    assert(device && commands && "submit received a null device or command list");
    assert((!device || !commands || commands->state == device) &&
           "command list belongs to a different device");
    if (!device || !commands || commands->state != device)
        abort_api_failure();

    auto* owned = commands;
    if (owned->recording)
    {
        assert(!owned->rendering && "cannot submit while a rendering scope is active");
        if (owned->rendering)
            abort_api_failure();
        require_vk(vkEndCommandBuffer(owned->command_buffer));
        owned->recording = false;
    }

    assert(owned->frame && owned->frame->active);
    if (!owned->frame || !owned->frame->active)
        abort_api_failure();
    assert(device->next_signal != 0 && "timeline submission counter overflow");
    if (device->next_signal == 0)
        abort_api_failure();
    const auto signal_value = device->next_signal;
    const VkCommandBufferSubmitInfo command_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = owned->command_buffer,
        .deviceMask = 1,
    };
    const VkSemaphoreSubmitInfo signal_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = device->timeline,
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
    require_vk(vkQueueSubmit2(device->queue, 1, &submit_info, VK_NULL_HANDLE));

    detail::TextureInitialization* initialization =
        owned->pending_texture_initializations.first;
    while (initialization)
    {
        detail::TextureInitialization* next = initialization->next;
        assert(initialization && !initialization->initialized);
        assert(initialization->owner == &owned->pending_texture_initializations);
        if (!initialization || initialization->initialized ||
            initialization->owner != &owned->pending_texture_initializations)
        {
            abort_api_failure();
        }
        initialization->initialized = true;
        initialization->owner = nullptr;
        initialization->previous = nullptr;
        initialization->next = nullptr;
        initialization = next;
    }
    owned->pending_texture_initializations = {};
    detail::FrameContext* frame = owned->frame;
    frame->last_signal = signal_value;
    frame->active = false;
    device->last_submitted_signal = signal_value;
    ++device->next_signal;
    ++device->next_frame;
    *owned = {};
}

void submit_and_wait(Device* device, CommandList* commands) noexcept
{
    submit(device, commands);
    const auto value = device->last_submitted_signal;
    const VkSemaphoreWaitInfo wait_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &device->timeline,
        .pValues = &value,
    };
    require_vk(vkWaitSemaphores(
        device->device,
        &wait_info,
        std::numeric_limits<std::uint64_t>::max()));
    for (auto& frame : device->frames)
    {
        if (frame.last_signal != 0 && frame.last_signal <= value)
        {
            assert(!frame.active);
            frame.last_signal = 0;
            device->reset_frame_commands(frame);
        }
    }
}

void wait_idle(Device* device) noexcept
{
    assert(device && "wait_idle called with a null device");
    if (!device)
        abort_api_failure();
    const bool commands_recording = has_active_commands(*device);
    assert(!commands_recording &&
           "wait_idle is not allowed while a command list is recording");
    if (commands_recording)
        abort_api_failure();
    require_vk(vkDeviceWaitIdle(device->device));
    for (auto& frame : device->frames)
    {
        frame.last_signal = 0;
        device->reset_frame_commands(frame);
    }
}

const DeviceCaps& get_device_caps(const Device* device) noexcept
{
    assert(device && "get_device_caps called with a null device");
    if (!device)
        abort_api_failure();
    return device->caps;
}

void destroy_device(Device* device) noexcept
{
    delete device;
}

void destroy_texture(Texture* texture) noexcept
{
    delete texture;
}

void destroy_pipeline(Pipeline* pipeline) noexcept
{
    delete pipeline;
}

void destroy_command_list(CommandList* commands) noexcept
{
    if (!commands || !commands->state)
        return;
    if (commands->pending_texture_initializations.first)
    {
        detail::transfer_texture_initializations(
            commands->state->pending_texture_initializations,
            commands->pending_texture_initializations);
    }
    if (commands->frame)
    {
        assert(commands->frame->last_signal == 0);
        commands->state->reset_frame_commands(*commands->frame);
        commands->frame->active = false;
    }
    *commands = {};
}

void bind_pipeline(CommandList* commands, const Pipeline* pipeline) noexcept
{
    const bool valid = commands && commands->recording && pipeline;
    assert(valid && "bind_pipeline received an empty command list or pipeline");
    if (!valid)
        abort_api_failure();
    const bool same_device = pipeline->state == commands->state;
    assert(same_device && "pipeline belongs to a different device");
    if (!same_device)
        abort_api_failure();
    vkCmdBindPipeline(
        commands->command_buffer, pipeline->bind_point, pipeline->pipeline);
    if (pipeline->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
        commands->bound_graphics = pipeline;
    else
        commands->bound_compute = pipeline;
}

AddressRange validate_range(CommandList* commands, const GpuRange& range) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "address commands require a recording command list");
    if (!valid)
        abort_api_failure();
    const bool nonempty = range.gpu && range.size != 0;
    assert(nonempty && "GPU range must have a non-null pointer and non-zero size");
    if (!nonempty)
        abort_api_failure();
    const auto address = static_cast<VkDeviceAddress>(
        reinterpret_cast<std::uintptr_t>(range.gpu));
    const bool address_fits =
        address <= std::numeric_limits<VkDeviceAddress>::max() - range.size;
    assert(address_fits && "GPU range address overflow");
    if (!address_fits)
        abort_api_failure();
    return {
        .address = address,
        .size = range.size,
    };
}

void validate_texture(CommandList* commands, Texture* texture) noexcept
{
    const bool valid = texture && texture->state == commands->state;
    assert(valid && "texture is empty or belongs to a different device");
    if (!valid)
        abort_api_failure();
}

void require_graphics_pipeline(CommandList* commands) noexcept
{
    assert(commands->bound_graphics && "draw requires a bound graphics pipeline");
    if (!commands->bound_graphics)
        abort_api_failure();
    const bool formats_match =
        commands->bound_graphics->color_format == commands->rendering_color_format &&
        commands->bound_graphics->depth_enabled == commands->rendering_has_depth &&
        (!commands->rendering_has_depth ||
         commands->bound_graphics->depth_format == commands->rendering_depth_format);
    assert(formats_match &&
           "graphics pipeline formats do not match the active rendering attachments");
    if (!formats_match)
        abort_api_failure();
}

void require_compute_pipeline(CommandList* commands) noexcept
{
    assert(commands->bound_compute && "dispatch requires a bound compute pipeline");
    if (!commands->bound_compute)
        abort_api_failure();
}

void emit_root_data(CommandList* commands, const void* data, std::size_t size) noexcept
{
    const bool has_data = data != nullptr;
    const bool valid = has_data == (size != 0) &&
                       (!has_data || ((size & 3u) == 0 &&
                                      size <= commands->state->heap_properties.maxPushDataSize));
    assert(valid &&
           "draw/dispatch root data must be null with zero size or non-null, "
           "four-byte sized, and fit "
           "VkPhysicalDeviceDescriptorHeapPropertiesEXT::maxPushDataSize");
    if (!valid)
        abort_api_failure();
    if (!has_data)
        return;
    const VkPushDataInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .pNext = nullptr,
        .offset = 0,
        .data = {
            .address = data,
            .size = size,
        },
    };
    commands->state->fn.cmd_push_data(commands->command_buffer, &info);
}

void set_resource_heap(CommandList* commands, const GpuRange& heap) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "set_resource_heap requires a recording command list");
    if (!valid)
        abort_api_failure();
    const auto& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(
        heap,
        properties.resourceHeapAlignment,
        properties.imageDescriptorAlignment > properties.bufferDescriptorAlignment
            ? properties.imageDescriptorAlignment
            : properties.bufferDescriptorAlignment,
        properties.minResourceHeapReservedRange,
        properties.maxResourceHeapSize,
        bind_info);
    commands->state->fn.cmd_bind_resource_heap(commands->command_buffer, &bind_info);
}

void set_sampler_heap(CommandList* commands, const GpuRange& heap) noexcept
{
    const bool valid = commands && commands->recording;
    assert(valid && "set_sampler_heap requires a recording command list");
    if (!valid)
        abort_api_failure();
    const auto& properties = commands->state->heap_properties;
    VkBindHeapInfoEXT bind_info{};
    make_heap_bind_info(heap,
                        properties.samplerHeapAlignment,
                        properties.samplerDescriptorAlignment,
                        properties.minSamplerHeapReservedRange,
                        properties.maxSamplerHeapSize,
                        bind_info);
    commands->state->fn.cmd_bind_sampler_heap(commands->command_buffer, &bind_info);
}

void begin_rendering(CommandList* commands,
                     Texture* color,
                     const float4& clear_color,
                     bool clear,
                     Texture* depth,
                     float clear_depth) noexcept
{
    const bool valid = commands && commands->recording && color;
    assert(valid && "begin_rendering received an empty object");
    if (!valid)
        abort_api_failure();
    assert(!commands->rendering && "a rendering scope is already active");
    const bool valid_color = color->state == commands->state &&
                             has_flag(color->usage,
                                      TextureUsage::color_attachment);
    assert(valid_color &&
           "render target must belong to the device and support color attachments");
    if (commands->rendering || !valid_color)
        abort_api_failure();
    if (depth)
    {
        const bool valid_depth = depth->state == commands->state;
        assert(valid_depth && "depth target is empty or belongs to a different device");
        if (!valid_depth)
            abort_api_failure();
        const bool depth_usage =
            has_flag(depth->usage, TextureUsage::depth_attachment) &&
            depth->format == Format::d32_float;
        assert(depth_usage &&
               "depth target must be a D32 texture created for depth-attachment use");
        const bool dimensions_match =
            depth->width == color->width &&
            depth->height == color->height;
        assert(dimensions_match && "color and depth target dimensions differ");
        if (!depth_usage || !dimensions_match)
            abort_api_failure();
    }
    const bool valid_clear_depth = !depth || !clear ||
                                   (clear_depth >= 0.0f && clear_depth <= 1.0f);
    assert(valid_clear_depth && "clear depth must be in the [0, 1] range");
    if (!valid_clear_depth)
        abort_api_failure();

    validate_texture(commands, color);
    if (depth)
        validate_texture(commands, depth);

    const VkClearValue color_clear{
        .color = {
            .float32 = {
                clear_color.x,
                clear_color.y,
                clear_color.z,
                clear_color.w,
            },
        },
    };
    const VkClearValue depth_clear{
        .depthStencil = {
            .depth = clear_depth,
            .stencil = 0,
        },
    };
    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = color->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = color_clear,
    };
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depth ? depth->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = depth_clear,
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {
            .offset = {
                .x = 0,
                .y = 0,
            },
            .extent = {
                .width = color->width,
                .height = color->height,
            },
        },
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = depth ? &depth_attachment : nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(commands->command_buffer, &rendering_info);

    const VkViewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(color->width),
        .height = static_cast<float>(color->height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor{
        .offset = {
            .x = 0,
            .y = 0,
        },
        .extent = {
            .width = color->width,
            .height = color->height,
        },
    };
    vkCmdSetViewport(commands->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(commands->command_buffer, 0, 1, &scissor);
    commands->rendering = true;
    commands->rendering_color_format = color->format;
    commands->rendering_has_depth = depth != nullptr;
    if (depth)
        commands->rendering_depth_format = depth->format;
}

void end_rendering(CommandList* commands) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "end_rendering requires an active rendering scope");
    if (!valid)
        abort_api_failure();
    vkCmdEndRendering(commands->command_buffer);
    commands->rendering = false;
    commands->rendering_has_depth = false;
}

void detail::draw_impl(CommandList* commands,
                       const void* root,
                       std::size_t root_size,
                       std::uint32_t vertex_count,
                       std::uint32_t instance_count,
                       std::uint32_t first_vertex,
                       std::uint32_t first_instance) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw requires an active rendering scope");
    if (!valid)
        abort_api_failure();
    require_graphics_pipeline(commands);
    emit_root_data(commands, root, root_size);
    vkCmdDraw(commands->command_buffer,
              vertex_count,
              instance_count,
              first_vertex,
              first_instance);
}

void detail::draw_indexed_impl(CommandList* commands,
                               const void* root,
                               std::size_t root_size,
                               const GpuRange& indices,
                               IndexType type,
                               std::uint32_t index_count,
                               std::uint32_t instance_count,
                               std::uint32_t first_index,
                               std::int32_t vertex_offset,
                               std::uint32_t first_instance) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indexed requires an active rendering scope");
    if (!valid)
        abort_api_failure();
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    if (!valid_type)
        abort_api_failure();
    require_graphics_pipeline(commands);
    const auto range = validate_range(commands, indices);
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
        abort_api_failure();

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = range.address,
            .size = range.size,
        },
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    emit_root_data(commands, root, root_size);
    vkCmdDrawIndexed(commands->command_buffer,
                     index_count,
                     instance_count,
                     first_index,
                     vertex_offset,
                     first_instance);
}

void detail::draw_indirect_impl(CommandList* commands,
                                const void* root,
                                std::size_t root_size,
                                const GpuRange& arguments,
                                std::uint32_t draw_count,
                                std::uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indirect requires an active rendering scope");
    if (!valid)
        abort_api_failure();
    require_graphics_pipeline(commands);
    if (stride == 0)
        stride = sizeof(VkDrawIndirectCommand);
    const auto required_size = draw_count == 0
                                   ? 0ull
                                   : static_cast<std::uint64_t>(draw_count - 1) * stride +
                                         sizeof(VkDrawIndirectCommand);
    const bool valid_arguments =
        draw_count != 0 && stride >= sizeof(VkDrawIndirectCommand) &&
        (stride & 3u) == 0 &&
        draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments && "indirect draw range, count, or stride is invalid");
    if (!valid_arguments)
        abort_api_failure();
    const auto range = validate_range(commands, arguments);
    const bool range_fits = (range.address & 3u) == 0 &&
                            range.size >= required_size && stride <= range.size;
    assert(range_fits && "indirect draw range, count, or stride is invalid");
    if (!range_fits)
        abort_api_failure();
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = range.address,
            .size = range.size,
            .stride = stride,
        },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    emit_root_data(commands, root, root_size);
    commands->state->fn.cmd_draw_indirect(commands->command_buffer, &info);
}

void detail::draw_indexed_indirect_impl(CommandList* commands,
                                        const void* root,
                                        std::size_t root_size,
                                        const GpuRange& indices,
                                        IndexType type,
                                        const GpuRange& arguments,
                                        std::uint32_t draw_count,
                                        std::uint32_t stride) noexcept
{
    const bool valid = commands && commands->recording && commands->rendering;
    assert(valid && "draw_indexed_indirect requires an active rendering scope");
    if (!valid)
        abort_api_failure();
    const bool valid_type = type == IndexType::uint16 || type == IndexType::uint32;
    assert(valid_type && "unknown index type");
    if (!valid_type)
        abort_api_failure();
    require_graphics_pipeline(commands);
    const auto index_range = validate_range(commands, indices);
    const auto index_alignment = type == IndexType::uint16 ? 2u : 4u;
    const bool valid_index_range = index_range.address % index_alignment == 0;
    assert(valid_index_range && "index address range is empty or misaligned");
    if (!valid_index_range)
        abort_api_failure();
    if (stride == 0)
        stride = sizeof(VkDrawIndexedIndirectCommand);
    const auto required_size = draw_count == 0
                                   ? 0ull
                                   : static_cast<std::uint64_t>(draw_count - 1) * stride +
                                         sizeof(VkDrawIndexedIndirectCommand);
    const bool valid_arguments =
        draw_count != 0 && stride >= sizeof(VkDrawIndexedIndirectCommand) &&
        (stride & 3u) == 0 &&
        draw_count <= commands->state->physical_properties.limits.maxDrawIndirectCount;
    assert(valid_arguments &&
           "indexed indirect draw range, count, or stride is invalid");
    if (!valid_arguments)
        abort_api_failure();
    const auto argument_range = validate_range(commands, arguments);
    const bool range_fits = (argument_range.address & 3u) == 0 &&
                            argument_range.size >= required_size &&
                            stride <= argument_range.size;
    assert(range_fits && "indexed indirect draw range, count, or stride is invalid");
    if (!range_fits)
        abort_api_failure();

    const VkBindIndexBuffer3InfoKHR bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = index_range.address,
            .size = index_range.size,
        },
        .addressFlags = address_flags,
        .indexType = type == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
    };
    commands->state->fn.cmd_bind_index_buffer(commands->command_buffer, &bind_info);
    const VkDrawIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = argument_range.address,
            .size = argument_range.size,
            .stride = stride,
        },
        .addressFlags = address_flags,
        .drawCount = draw_count,
    };
    emit_root_data(commands, root, root_size);
    commands->state->fn.cmd_draw_indexed_indirect(commands->command_buffer, &info);
}

void detail::dispatch_impl(CommandList* commands,
                           const void* root,
                           std::size_t root_size,
                           std::uint32_t x,
                           std::uint32_t y,
                           std::uint32_t z) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "dispatch requires a recording command list outside rendering");
    if (!valid)
        abort_api_failure();
    require_compute_pipeline(commands);
    const auto& limits = commands->state->physical_properties.limits.maxComputeWorkGroupCount;
    const bool count_fits = x <= limits[0] && y <= limits[1] && z <= limits[2];
    assert(count_fits &&
           "dispatch group count exceeds VkPhysicalDeviceLimits::maxComputeWorkGroupCount");
    if (!count_fits)
        abort_api_failure();
    emit_root_data(commands, root, root_size);
    vkCmdDispatch(commands->command_buffer, x, y, z);
}

void detail::dispatch_indirect_impl(CommandList* commands,
                                    const void* root,
                                    std::size_t root_size,
                                    const GpuRange& arguments) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid &&
           "dispatch_indirect requires a recording command list outside rendering");
    if (!valid)
        abort_api_failure();
    require_compute_pipeline(commands);
    const auto range = validate_range(commands, arguments);
    const bool range_fits = (range.address & 3u) == 0 &&
                            range.size >= sizeof(VkDispatchIndirectCommand);
    assert(range_fits && "indirect dispatch range is too small or misaligned");
    if (!range_fits)
        abort_api_failure();
    const VkDispatchIndirect2InfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = range.address,
            .size = range.size,
        },
        .addressFlags = address_flags,
    };
    emit_root_data(commands, root, root_size);
    commands->state->fn.cmd_dispatch_indirect(commands->command_buffer, &info);
}

void copy_memory(CommandList* commands,
                 const GpuRange& source,
                 const GpuRange& destination) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "copy_memory requires a recording command list outside rendering");
    if (!valid)
        abort_api_failure();
    const auto source_range = validate_range(commands, source);
    const auto destination_range = validate_range(commands, destination);
    const bool destination_fits = destination_range.size >= source_range.size;
    assert(destination_fits &&
           "copy_memory destination range is smaller than its source range");
    const bool overlaps =
        source_range.address < destination_range.address + source_range.size &&
        destination_range.address < source_range.address + source_range.size;
    assert(!overlaps && "copy_memory source and destination ranges overlap");
    if (!destination_fits || overlaps)
        abort_api_failure();

    const VkDeviceMemoryCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .pNext = nullptr,
        .srcRange = {
            .address = source_range.address,
            .size = source_range.size,
        },
        .srcFlags = address_flags,
        .dstRange = {
            .address = destination_range.address,
            .size = destination_range.size,
        },
        .dstFlags = address_flags,
    };
    const VkCopyDeviceMemoryInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        .pNext = nullptr,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_memory(commands->command_buffer, &info);
}

void copy_memory_to_texture(CommandList* commands,
                            const GpuRange& source,
                            Texture* destination) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering && destination;
    assert(valid &&
           "copy_memory_to_texture received an empty object or active rendering scope");
    if (!valid)
        abort_api_failure();
    const bool valid_texture = destination->state == commands->state &&
                               has_flag(destination->usage,
                                        TextureUsage::transfer_destination);
    assert(valid_texture &&
           "texture must belong to the device and support transfer destination use");
    if (!valid_texture)
        abort_api_failure();
    validate_texture(commands, destination);
    const auto texel_size = bytes_per_texel(destination->format);
    const auto required_size = base_level_byte_size(
        destination->format,
        destination->width,
        destination->height,
        destination->depth);
    const auto source_range = validate_range(commands, source);
    const bool range_fits = source_range.address % texel_size == 0 &&
                            source_range.size >= required_size;
    assert(range_fits &&
           "source range is empty, misaligned, or too small for the texture base level");
    if (!range_fits)
        abort_api_failure();

    const bool depth = destination->format == Format::d32_float;
    const VkDeviceMemoryImageCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = source_range.address,
            .size = source_range.size,
        },
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
        .imageOffset = {
            .x = 0,
            .y = 0,
            .z = 0,
        },
        .imageExtent = {
            .width = destination->width,
            .height = destination->height,
            .depth = destination->depth,
        },
    };
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .pNext = nullptr,
        .image = destination->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_memory_to_image(commands->command_buffer, &info);
}

void copy_texture_to_memory(CommandList* commands,
                            Texture* source,
                            const GpuRange& destination) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering && source;
    assert(valid &&
           "copy_texture_to_memory received an empty object or active rendering scope");
    if (!valid)
        abort_api_failure();
    const bool valid_texture = source->state == commands->state &&
                               has_flag(source->usage,
                                        TextureUsage::transfer_source);
    assert(valid_texture &&
           "texture must belong to the device and support transfer source use");
    if (!valid_texture)
        abort_api_failure();
    validate_texture(commands, source);
    const auto texel_size = bytes_per_texel(source->format);
    const auto required_size = base_level_byte_size(
        source->format,
        source->width,
        source->height,
        source->depth);
    const auto destination_range = validate_range(commands, destination);
    const bool range_fits = destination_range.address % texel_size == 0 &&
                            destination_range.size >= required_size;
    assert(range_fits &&
           "destination range is empty, misaligned, or too small for the texture base level");
    if (!range_fits)
        abort_api_failure();

    const bool depth = source->format == Format::d32_float;
    const VkDeviceMemoryImageCopyKHR region{
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .pNext = nullptr,
        .addressRange = {
            .address = destination_range.address,
            .size = destination_range.size,
        },
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
        .imageOffset = {
            .x = 0,
            .y = 0,
            .z = 0,
        },
        .imageExtent = {
            .width = source->width,
            .height = source->height,
            .depth = source->depth,
        },
    };
    const VkCopyDeviceMemoryImageInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .pNext = nullptr,
        .image = source->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.cmd_copy_image_to_memory(commands->command_buffer, &info);
}

void barrier(CommandList* commands,
             Stage before,
             Access before_access,
             Stage after,
             Access after_access) noexcept
{
    const bool valid = commands && commands->recording && !commands->rendering;
    assert(valid && "barrier requires a recording command list outside rendering");
    if (!valid)
        abort_api_failure();
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
    vkCmdPipelineBarrier2(commands->command_buffer, &dependency);
}

} // namespace gfx
