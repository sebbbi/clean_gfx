#include "example_support.hpp"

#include <cassert>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

const char* gfx_error_name(gfx::Error error) noexcept
{
    switch (error)
    {
    case gfx::Error::none: return "none";
    case gfx::Error::unsupported: return "unsupported";
    case gfx::Error::out_of_device_memory: return "out of device memory";
    case gfx::Error::device_lost: return "device lost";
    case gfx::Error::driver_error: return "driver error";
    }
    assert(false && "unknown gfx::Error");
    return "unknown error";
}

ShaderCode read_spirv(const char* application, const char* path) noexcept
{
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    fopen_s(&file, path, "rb");
#else
    std::FILE* file = std::fopen(path, "rb");
#endif
    if (!file)
    {
        std::fprintf(stderr, "%s: could not open shader: %s\n", application, path);
        return {};
    }
    char file_buffer[4096];
    std::setvbuf(file, file_buffer, _IOFBF, sizeof(file_buffer));
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fprintf(stderr, "%s: could not seek shader: %s\n", application, path);
        std::fclose(file);
        return {};
    }
    const long end = std::ftell(file);
    if (end <= 0 || end % static_cast<long>(sizeof(std::uint32_t)) != 0)
    {
        std::fprintf(stderr, "%s: invalid SPIR-V file: %s\n", application, path);
        std::fclose(file);
        return {};
    }
    const std::size_t byte_count = static_cast<std::size_t>(end);
    const std::size_t word_count =
        byte_count / sizeof(std::uint32_t);
    ShaderCode code{};
    if (word_count > code.words.size())
    {
        std::fprintf(stderr,
                     "%s: shader exceeds %zu byte example limit: %s\n",
                     application,
                     shader_word_capacity * sizeof(std::uint32_t),
                     path);
        std::fclose(file);
        return {};
    }
    std::rewind(file);
    const std::size_t words_read =
        std::fread(code.words.data(), sizeof(std::uint32_t), word_count, file);
    std::fclose(file);
    if (words_read != word_count)
    {
        std::fprintf(stderr, "%s: could not read shader: %s\n", application, path);
        return {};
    }
    code.size = word_count;
    return code;
}

bool write_bgra8_ppm(const char* application,
                     const char* path,
                     const std::byte* bgra,
                     std::uint32_t width,
                     std::uint32_t height) noexcept
{
    assert(path && bgra && width && height);
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    fopen_s(&file, path, "wb");
#else
    std::FILE* file = std::fopen(path, "wb");
#endif
    if (!file)
    {
        std::fprintf(stderr, "%s: could not create output: %s\n", application, path);
        return false;
    }
    char file_buffer[16384];
    std::setvbuf(file, file_buffer, _IOFBF, sizeof(file_buffer));
    if (std::fprintf(file, "P6\n%u %u\n255\n", width, height) < 0)
    {
        std::fprintf(stderr, "%s: could not write output: %s\n", application, path);
        std::fclose(file);
        return false;
    }
    constexpr std::size_t pixels_per_chunk = 1024;
    char rgb[3072];
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    for (std::size_t first = 0; first < pixel_count; first += pixels_per_chunk)
    {
        const std::size_t remaining = pixel_count - first;
        const std::size_t count =
            remaining < pixels_per_chunk ? remaining : pixels_per_chunk;
        for (std::size_t index = 0; index < count; ++index)
        {
            const std::byte* pixel = bgra + (first + index) * 4;
            rgb[index * 3 + 0] = static_cast<char>(pixel[2]);
            rgb[index * 3 + 1] = static_cast<char>(pixel[1]);
            rgb[index * 3 + 2] = static_cast<char>(pixel[0]);
        }
        if (std::fwrite(rgb, 3, count, file) != count)
        {
            std::fprintf(stderr, "%s: could not write output: %s\n", application, path);
            std::fclose(file);
            return false;
        }
    }
    if (std::fclose(file) != 0)
    {
        std::fprintf(stderr, "%s: could not write output: %s\n", application, path);
        return false;
    }
    return true;
}

#if defined(_WIN32)

namespace
{

constexpr const char* window_class_name = "clean_gfx_example_window";
constexpr DWORD window_style =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

LRESULT CALLBACK example_window_proc(HWND hwnd,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam) noexcept
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

} // namespace

ExampleWindow open_example_window(const char* title,
                                  std::uint32_t width,
                                  std::uint32_t height) noexcept
{
    assert(title && width && height);
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSEXA window_class{
        .cbSize = sizeof(WNDCLASSEXA),
        .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
        .lpfnWndProc = example_window_proc,
        .hInstance = instance,
        .hCursor = LoadCursorA(nullptr, IDC_ARROW),
        .lpszClassName = window_class_name,
    };
    if (!RegisterClassExA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return {};

    RECT rectangle{
        .right = static_cast<LONG>(width),
        .bottom = static_cast<LONG>(height),
    };
    if (!AdjustWindowRectEx(&rectangle, window_style, FALSE, 0))
        return {};

    const HWND hwnd = CreateWindowExA(
        0,
        window_class_name,
        title,
        window_style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd)
        return {};
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return {.handle = hwnd, .running = true};
}

bool pump_example_window(ExampleWindow& window) noexcept
{
    MSG message{};
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            window.handle = nullptr;
            window.running = false;
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return window.running;
}

bool present_bgra8(const ExampleWindow& window,
                   const std::byte* pixels,
                   std::uint32_t width,
                   std::uint32_t height) noexcept
{
    assert(window.handle && pixels && width && height);
    const HWND hwnd = static_cast<HWND>(window.handle);
    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return false;
    const HDC dc = GetDC(hwnd);
    if (!dc)
        return false;

    BITMAPINFO info{
        .bmiHeader = {
            .biSize = sizeof(BITMAPINFOHEADER),
            .biWidth = static_cast<LONG>(width),
            .biHeight = -static_cast<LONG>(height),
            .biPlanes = 1,
            .biBitCount = 32,
        },
    };
    const int result = StretchDIBits(
        dc,
        0,
        0,
        client.right - client.left,
        client.bottom - client.top,
        0,
        0,
        static_cast<int>(width),
        static_cast<int>(height),
        pixels,
        &info,
        DIB_RGB_COLORS,
        SRCCOPY);
    ReleaseDC(hwnd, dc);
    return result != 0 && result != static_cast<int>(GDI_ERROR);
}

void close_example_window(ExampleWindow& window) noexcept
{
    if (window.handle)
        DestroyWindow(static_cast<HWND>(window.handle));
    window = {};
}

#else

ExampleWindow open_example_window(const char*,
                                  std::uint32_t,
                                  std::uint32_t) noexcept
{
    return {};
}

bool pump_example_window(ExampleWindow&) noexcept
{
    return false;
}

bool present_bgra8(const ExampleWindow&,
                   const std::byte*,
                   std::uint32_t,
                   std::uint32_t) noexcept
{
    return false;
}

void close_example_window(ExampleWindow& window) noexcept
{
    window = {};
}

#endif
