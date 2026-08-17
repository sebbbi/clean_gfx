#include "example_support.hpp"

#include <cassert>
#include <fstream>
#include <iostream>

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

std::vector<std::uint32_t> read_spirv(const char* application,
                                      const char* path) noexcept
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        std::cerr << application << ": could not open shader: " << path << '\n';
        return {};
    }
    const std::streampos end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) % sizeof(std::uint32_t) != 0)
    {
        std::cerr << application << ": invalid SPIR-V file: " << path << '\n';
        return {};
    }
    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(end) / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), end);
    if (!file)
    {
        std::cerr << application << ": could not read shader: " << path << '\n';
        return {};
    }
    return words;
}

bool write_bgra8_ppm(const char* application,
                     const char* path,
                     const std::byte* bgra,
                     std::uint32_t width,
                     std::uint32_t height) noexcept
{
    assert(path && bgra && width && height);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        std::cerr << application << ": could not create output: " << path << '\n';
        return false;
    }
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(width) * height; ++i)
    {
        const std::byte* pixel = bgra + i * 4;
        const char rgb[3]{
            static_cast<char>(pixel[2]),
            static_cast<char>(pixel[1]),
            static_cast<char>(pixel[0]),
        };
        file.write(rgb, 3);
    }
    if (!file)
    {
        std::cerr << application << ": could not write output: " << path << '\n';
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
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = example_window_proc,
        .hInstance = instance,
        .hCursor = LoadCursorA(nullptr, IDC_ARROW),
        .lpszClassName = window_class_name,
    };
    if (!RegisterClassExA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return {};

    RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
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

bool present_bgra8(ExampleWindow& window,
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

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
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
    return result != 0 && result != GDI_ERROR;
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

bool present_bgra8(ExampleWindow&,
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
