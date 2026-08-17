// The cube mesh and presentation parameters are adapted from Vulkan-Tools'
// vkcube sample and modified for clean_gfx. Copyright notices and the
// Apache-2.0 license are in NOTICE.md and LICENSE-Apache-2.0.txt.

#include "cube_shared.h"

#include "example_support.hpp"

#include <clean_gfx/clean_gfx.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numbers>
#include <vector>

#if defined(_WIN32)
#include "lunarg_logo_png.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <wincodec.h>
#endif

namespace
{

constexpr char application_name[] = "clean_gfx_cube";
constexpr std::uint32_t width = 500;
constexpr std::uint32_t height = 500;
constexpr std::uint32_t texture_width = 256;
constexpr std::uint32_t texture_height = 256;
constexpr float radians_per_frame = 4.0f * std::numbers::pi_v<float> / 180.0f;

constexpr std::array<CubeVertex, 36> cube_vertices{{
    {{-1.0f, -1.0f, -1.0f, 1.0f}, {0.0f, 1.0f}},
    {{-1.0f, -1.0f,  1.0f, 1.0f}, {1.0f, 1.0f}},
    {{-1.0f,  1.0f,  1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-1.0f,  1.0f,  1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-1.0f,  1.0f, -1.0f, 1.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f, -1.0f, 1.0f}, {0.0f, 1.0f}},

    {{-1.0f, -1.0f, -1.0f, 1.0f}, {1.0f, 1.0f}},
    {{ 1.0f,  1.0f, -1.0f, 1.0f}, {0.0f, 0.0f}},
    {{ 1.0f, -1.0f, -1.0f, 1.0f}, {0.0f, 1.0f}},
    {{-1.0f, -1.0f, -1.0f, 1.0f}, {1.0f, 1.0f}},
    {{-1.0f,  1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 1.0f,  1.0f, -1.0f, 1.0f}, {0.0f, 0.0f}},

    {{-1.0f, -1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 1.0f, -1.0f, -1.0f, 1.0f}, {1.0f, 1.0f}},
    {{ 1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{-1.0f, -1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{-1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 0.0f}},

    {{-1.0f,  1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-1.0f,  1.0f,  1.0f, 1.0f}, {0.0f, 0.0f}},
    {{ 1.0f,  1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{-1.0f,  1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 1.0f,  1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 1.0f,  1.0f, -1.0f, 1.0f}, {1.0f, 1.0f}},

    {{ 1.0f,  1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 1.0f,  1.0f,  1.0f, 1.0f}, {0.0f, 0.0f}},
    {{ 1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 1.0f, -1.0f, -1.0f, 1.0f}, {1.0f, 1.0f}},
    {{ 1.0f,  1.0f, -1.0f, 1.0f}, {1.0f, 0.0f}},

    {{-1.0f,  1.0f,  1.0f, 1.0f}, {0.0f, 0.0f}},
    {{-1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 1.0f,  1.0f,  1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-1.0f, -1.0f,  1.0f, 1.0f}, {0.0f, 1.0f}},
    {{ 1.0f, -1.0f,  1.0f, 1.0f}, {1.0f, 1.0f}},
    {{ 1.0f,  1.0f,  1.0f, 1.0f}, {1.0f, 0.0f}},
}};

struct Matrix
{
    float element[4][4]{};
};

float3 subtract(float3 lhs, float3 rhs) noexcept
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

float dot(float3 lhs, float3 rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

float3 cross(float3 lhs, float3 rhs) noexcept
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

float3 normalize(float3 value) noexcept
{
    const float reciprocal_length = 1.0f / std::sqrt(dot(value, value));
    return {
        value.x * reciprocal_length,
        value.y * reciprocal_length,
        value.z * reciprocal_length,
    };
}

Matrix multiply(const Matrix& lhs, const Matrix& rhs) noexcept
{
    Matrix result{};
    for (std::uint32_t column = 0; column != 4; ++column)
    {
        for (std::uint32_t row = 0; row != 4; ++row)
        {
            for (std::uint32_t inner = 0; inner != 4; ++inner)
            {
                result.element[column][row] +=
                    lhs.element[inner][row] * rhs.element[column][inner];
            }
        }
    }
    return result;
}

Matrix rotation_y(float angle) noexcept
{
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    Matrix result{};
    result.element[0][0] = cosine;
    result.element[0][2] = -sine;
    result.element[1][1] = 1.0f;
    result.element[2][0] = sine;
    result.element[2][2] = cosine;
    result.element[3][3] = 1.0f;
    return result;
}

Matrix perspective(float vertical_field_of_view,
                   float aspect,
                   float near_plane,
                   float far_plane) noexcept
{
    const float focal_length = 1.0f / std::tan(vertical_field_of_view * 0.5f);
    Matrix result{};
    result.element[0][0] = focal_length / aspect;
    result.element[1][1] = -focal_length;
    result.element[2][2] = far_plane / (near_plane - far_plane);
    result.element[2][3] = -1.0f;
    result.element[3][2] = near_plane * far_plane / (near_plane - far_plane);
    return result;
}

Matrix look_at(float3 eye, float3 center, float3 up) noexcept
{
    const float3 forward = normalize(subtract(center, eye));
    const float3 side = normalize(cross(forward, up));
    const float3 camera_up = cross(side, forward);

    Matrix result{};
    result.element[0][0] = side.x;
    result.element[0][1] = camera_up.x;
    result.element[0][2] = -forward.x;
    result.element[1][0] = side.y;
    result.element[1][1] = camera_up.y;
    result.element[1][2] = -forward.y;
    result.element[2][0] = side.z;
    result.element[2][1] = camera_up.z;
    result.element[2][2] = -forward.z;
    result.element[3][0] = -dot(side, eye);
    result.element[3][1] = -dot(camera_up, eye);
    result.element[3][2] = dot(forward, eye);
    result.element[3][3] = 1.0f;
    return result;
}

float3x4 shader_matrix(const Matrix& source) noexcept
{
    float3x4 result{};
    for (std::uint32_t row = 0; row != 2; ++row)
    {
        result.rows[row] = {
            source.element[0][row],
            source.element[1][row],
            source.element[2][row],
            source.element[3][row],
        };
    }
    result.rows[2] = {
        source.element[0][3],
        source.element[1][3],
        source.element[2][3],
        source.element[3][3],
    };
    return result;
}

#if defined(_WIN32)

int base64_value(char character) noexcept
{
    if (character >= 'A' && character <= 'Z')
        return character - 'A';
    if (character >= 'a' && character <= 'z')
        return character - 'a' + 26;
    if (character >= '0' && character <= '9')
        return character - '0' + 52;
    if (character == '+')
        return 62;
    if (character == '/')
        return 63;
    if (character == '=')
        return -2;
    return -1;
}

std::vector<std::byte> decode_base64() noexcept
{
    constexpr std::size_t length = sizeof(lunarg_logo_png_base64) - 1;
    if (length == 0 || length % 4 != 0)
        return {};

    std::vector<std::byte> output;
    output.reserve(length / 4 * 3);
    for (std::size_t offset = 0; offset != length; offset += 4)
    {
        const int a = base64_value(lunarg_logo_png_base64[offset + 0]);
        const int b = base64_value(lunarg_logo_png_base64[offset + 1]);
        const int c = base64_value(lunarg_logo_png_base64[offset + 2]);
        const int d = base64_value(lunarg_logo_png_base64[offset + 3]);
        const bool last_group = offset + 4 == length;
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && !last_group))
            return {};

        const std::uint32_t bits = (static_cast<std::uint32_t>(a) << 18u) |
                                   (static_cast<std::uint32_t>(b) << 12u) |
                                   (static_cast<std::uint32_t>(c < 0 ? 0 : c) << 6u) |
                                   static_cast<std::uint32_t>(d < 0 ? 0 : d);
        output.push_back(static_cast<std::byte>((bits >> 16u) & 0xffu));
        if (c != -2)
            output.push_back(static_cast<std::byte>((bits >> 8u) & 0xffu));
        if (d != -2)
            output.push_back(static_cast<std::byte>(bits & 0xffu));
    }
    return output;
}

template<typename T>
void release_com(T*& object) noexcept
{
    if (object)
        object->Release();
    object = nullptr;
}

std::vector<std::byte> make_texture() noexcept
{
    std::vector<std::byte> png = decode_base64();
    if (png.empty())
        return {};

    std::vector<std::byte> pixels;

    const HRESULT initialize_result =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialize_result);
    HRESULT result = initialize_result == RPC_E_CHANGED_MODE
                         ? S_OK
                         : initialize_result;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    if (SUCCEEDED(result))
    {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    }
    if (SUCCEEDED(result))
        result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
    {
        result = stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(png.data()),
            static_cast<DWORD>(png.size()));
    }
    if (SUCCEEDED(result))
    {
        result = factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(result))
        result = decoder->GetFrame(0, &frame);

    UINT decoded_width = 0;
    UINT decoded_height = 0;
    if (SUCCEEDED(result))
        result = frame->GetSize(&decoded_width, &decoded_height);
    if (SUCCEEDED(result) &&
        (decoded_width != texture_width || decoded_height != texture_height))
    {
        result = WINCODEC_ERR_BADIMAGE;
    }
    if (SUCCEEDED(result))
        result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result))
    {
        result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(result))
    {
        pixels.resize(static_cast<std::size_t>(texture_width) *
                      texture_height * 4);
        result = converter->CopyPixels(
            nullptr, texture_width * 4,
            static_cast<UINT>(pixels.size()),
            reinterpret_cast<BYTE*>(pixels.data()));
    }

    release_com(converter);
    release_com(frame);
    release_com(decoder);
    release_com(stream);
    release_com(factory);
    if (uninitialize)
        CoUninitialize();
    if (FAILED(result))
        return {};
    return pixels;
}

#else

std::vector<std::byte> make_texture() noexcept
{
    std::vector<std::byte> pixels;
    pixels.resize(
        static_cast<std::size_t>(texture_width) * texture_height * 4);
    for (std::uint32_t y = 0; y != texture_height; ++y)
    {
        for (std::uint32_t x = 0; x != texture_width; ++x)
        {
            const bool light_square = ((x / 32u) ^ (y / 32u)) % 2u == 0u;
            std::uint8_t red = light_square ? 224u : 44u;
            std::uint8_t green = light_square ? 224u : 56u;
            std::uint8_t blue = light_square ? 224u : 72u;

            const bool blue_stripe = x + y > 224u && x + y < 288u;
            const bool orange_stripe = x + (texture_height - 1u - y) > 224u &&
                                       x + (texture_height - 1u - y) < 288u;
            if (blue_stripe)
            {
                red = 40u;
                green = 128u;
                blue = 232u;
            }
            if (orange_stripe)
            {
                red = 240u;
                green = 128u;
                blue = 36u;
            }

            const std::size_t offset =
                (static_cast<std::size_t>(y) * texture_width + x) * 4;
            pixels[offset + 0] = static_cast<std::byte>(red);
            pixels[offset + 1] = static_cast<std::byte>(green);
            pixels[offset + 2] = static_cast<std::byte>(blue);
            pixels[offset + 3] = static_cast<std::byte>(255u);
        }
    }
    return pixels;
}

#endif

void render_frame(gfx::Device* device,
                  gfx::Pipeline* pipeline,
                  gfx::Texture* target,
                  gfx::Texture* depth,
                  gfx::GpuRange resource_heap,
                  gfx::GpuRange sampler_heap,
                  const CubeRootArguments& root,
                  gfx::GpuAllocation<std::byte> readback) noexcept
{
    gfx::CommandList* commands = gfx::begin_commands(device);
    gfx::barrier(commands,
                 gfx::Stage::transfer | gfx::Stage::depth_tests,
                 gfx::Access::transfer_read | gfx::Access::depth_write,
                 gfx::Stage::color_output | gfx::Stage::depth_tests,
                 gfx::Access::color_write | gfx::Access::depth_write);
    gfx::set_resource_heap(commands, resource_heap);
    gfx::set_sampler_heap(commands, sampler_heap);
    gfx::begin_rendering(
        commands, target, {0.2f, 0.2f, 0.2f, 0.2f}, true, depth, 1.0f);
    gfx::bind_pipeline(commands, pipeline);
    gfx::draw(commands, &root, static_cast<std::uint32_t>(cube_vertices.size()));
    gfx::end_rendering(commands);
    gfx::barrier(commands,
                 gfx::Stage::color_output,
                 gfx::Access::color_write,
                 gfx::Stage::transfer,
                 gfx::Access::transfer_read);
    gfx::copy_texture_to_memory(commands, target, gfx::gpu_range(readback));
    gfx::barrier(commands,
                 gfx::Stage::transfer,
                 gfx::Access::transfer_write,
                 gfx::Stage::host,
                 gfx::Access::host_read);
    gfx::submit_and_wait(device, commands);
}

} // namespace

int main(int argc, char** argv)
{
    const char* output_path = argc > 1 ? argv[1] : nullptr;
    if (!output_path && !example_window_supported)
    {
        std::cerr << application_name
                  << ": interactive presentation is only available on Windows; "
                     "pass an output PPM path instead\n";
        return 1;
    }

    const std::vector<std::byte> texture_pixels = make_texture();
    if (texture_pixels.empty())
    {
        std::cerr << application_name << ": could not decode embedded cube texture\n";
        return 1;
    }

    const std::vector<std::uint32_t> vertex_spirv =
        read_spirv(application_name, CLEAN_GFX_CUBE_VERTEX_SPV_PATH);
    const std::vector<std::uint32_t> fragment_spirv =
        read_spirv(application_name, CLEAN_GFX_CUBE_FRAGMENT_SPV_PATH);
    if (vertex_spirv.empty() || fragment_spirv.empty())
        return 1;

    const gfx::DeviceInit device_init = gfx::create_device({
        .application_name = application_name,
    });
    if (device_init.error != gfx::Error::none)
    {
        std::cerr << application_name << ": create device: "
                  << gfx_error_name(device_init.error) << '\n';
        return 1;
    }
    gfx::Device* device = device_init.device;
    const gfx::DeviceCaps caps = gfx::get_device_caps(device);
    std::cout << "Using " << caps.device_name << '\n';

    const std::uint64_t resource_heap_size = caps.image_descriptor_size;
    const std::uint64_t sampler_heap_size =
        caps.sampler_descriptor_size *
        static_cast<std::uint32_t>(CubeSampler::count);
    gfx::GpuAllocation<std::byte> resource_heap =
        gfx::gpu_malloc_resource_heap(device, resource_heap_size);
    gfx::GpuAllocation<std::byte> sampler_heap =
        gfx::gpu_malloc_sampler_heap(device, sampler_heap_size);

    gfx::GpuAllocation<CubeVertex> vertex_memory =
        gfx::gpu_malloc<CubeVertex>(device, cube_vertices.size());
    std::memcpy(vertex_memory.cpu, cube_vertices.data(), sizeof(cube_vertices));

    gfx::GpuAllocation<std::byte> texture_upload =
        gfx::gpu_malloc<std::byte>(device, texture_pixels.size());
    std::memcpy(texture_upload.cpu, texture_pixels.data(), texture_pixels.size());

    gfx::Texture* texture = gfx::create_texture(
        device,
        {
            .width = texture_width,
            .height = texture_height,
            .depth = 1,
            .mip_levels = 1,
            .format = gfx::Format::rgba8_srgb,
            .usage = gfx::TextureUsage::sampled |
                     gfx::TextureUsage::transfer_destination,
            .name = "cube texture",
        });

    gfx::write_texture_descriptor(device,
                                  resource_heap.cpu,
                                  texture,
                                  gfx::TextureDescriptorType::sampled);

    const std::array<gfx::SamplerDesc,
                     static_cast<std::size_t>(CubeSampler::count)> sampler_descs{{
        {},
        {
            .min_filter = gfx::Filter::nearest,
            .mag_filter = gfx::Filter::nearest,
        },
        {
            .address_u = gfx::AddressMode::clamp_to_edge,
            .address_v = gfx::AddressMode::clamp_to_edge,
            .address_w = gfx::AddressMode::clamp_to_edge,
        },
        {
            .min_filter = gfx::Filter::nearest,
            .mag_filter = gfx::Filter::nearest,
            .address_u = gfx::AddressMode::clamp_to_edge,
            .address_v = gfx::AddressMode::clamp_to_edge,
            .address_w = gfx::AddressMode::clamp_to_edge,
        },
    }};
    std::byte* sampler_descriptors = sampler_heap.cpu;
    for (std::size_t index = 0; index < sampler_descs.size(); ++index)
    {
        gfx::write_sampler_descriptor(
            device,
            sampler_descriptors + index * caps.sampler_descriptor_size,
            sampler_descs[index]);
    }

    gfx::Texture* target = gfx::create_texture(
        device,
        {
            .width = width,
            .height = height,
            .depth = 1,
            .mip_levels = 1,
            .format = gfx::Format::bgra8_unorm,
            .usage = gfx::TextureUsage::color_attachment |
                     gfx::TextureUsage::transfer_source,
            .name = "cube color target",
        });

    gfx::Texture* depth = gfx::create_texture(
        device,
        {
            .width = width,
            .height = height,
            .depth = 1,
            .mip_levels = 1,
            .format = gfx::Format::d32_float,
            .usage = gfx::TextureUsage::depth_attachment,
            .name = "cube depth target",
        });

    gfx::GpuAllocation<std::byte> readback = gfx::gpu_malloc<std::byte>(
        device,
        static_cast<std::uint64_t>(width) * height * 4,
        gfx::MemoryType::readback);

    gfx::Pipeline* pipeline = gfx::create_graphics_pipeline(
        device,
        {
            .vertex_spirv = vertex_spirv,
            .fragment_spirv = fragment_spirv,
            .color_format = gfx::Format::bgra8_unorm,
            .depth_format = gfx::Format::d32_float,
            .depth_enabled = true,
            .depth_write = true,
            .topology = gfx::PrimitiveTopology::triangle_list,
            .cull = gfx::CullMode::clockwise,
            .name = "cube pipeline",
        });

    gfx::CommandList* upload_commands = gfx::begin_commands(device);
    gfx::copy_memory_to_texture(
        upload_commands, gfx::gpu_range(texture_upload), texture);
    gfx::barrier(
        upload_commands,
        gfx::Stage::transfer | gfx::Stage::host,
        gfx::Access::transfer_write | gfx::Access::host_write,
        gfx::Stage::vertex | gfx::Stage::fragment,
        gfx::Access::shader_read | gfx::Access::descriptor_read);
    gfx::submit_and_wait(device, upload_commands);
    gfx::gpu_free(device, texture_upload);

    const Matrix view = look_at({0.0f, 3.0f, 5.0f},
                                {0.0f, 0.0f, 0.0f},
                                {0.0f, 1.0f, 0.0f});
    const Matrix projection = perspective(
        45.0f * std::numbers::pi_v<float> / 180.0f,
        static_cast<float>(width) / static_cast<float>(height),
        0.1f,
        100.0f);

    ExampleWindow window = output_path
                               ? ExampleWindow{}
                               : open_example_window(
                                     "clean_gfx spinning textured cube", width, height);
    bool succeeded = output_path || window.running;
    if (!succeeded)
    {
        std::cerr << application_name << ": could not open window\n";
    }

    std::uint64_t frame_index = 0;
    if (succeeded)
    {
        do
        {
            ++frame_index;
            const Matrix model = rotation_y(
                radians_per_frame * static_cast<float>(frame_index));
            const Matrix mvp = multiply(projection, multiply(view, model));
            const CubeRootArguments root{
                .vertices = vertex_memory.gpu,
                .texture_index = 0,
                .transform = shader_matrix(mvp),
                .depth_transform = {
                    -projection.element[2][2],
                    projection.element[3][2],
                },
            };

            render_frame(device,
                         pipeline,
                         target,
                         depth,
                         gfx::gpu_range(resource_heap),
                         gfx::gpu_range(sampler_heap),
                         root,
                         readback);

            const std::byte* pixels = readback.cpu;
            if (output_path)
            {
                succeeded = write_bgra8_ppm(
                    application_name, output_path, pixels, width, height);
                if (succeeded)
                    std::cout << "Wrote " << output_path << '\n';
                break;
            }
            succeeded = present_bgra8(window, pixels, width, height);
#if defined(_WIN32)
            if (succeeded)
                Sleep(16);
#endif
        }
        while (succeeded && pump_example_window(window));
    }

    close_example_window(window);
    gfx::destroy_pipeline(pipeline);
    gfx::gpu_free(device, readback);
    gfx::destroy_texture(depth);
    gfx::destroy_texture(target);
    gfx::destroy_texture(texture);
    gfx::gpu_free(device, vertex_memory);
    gfx::gpu_free(device, sampler_heap);
    gfx::gpu_free(device, resource_heap);
    gfx::destroy_device(device);
    return succeeded ? 0 : 1;
}
