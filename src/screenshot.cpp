#include "screenshot.hpp"

#include "game_window.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

namespace tongs {
namespace {

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;  // top-down, 4 bytes per pixel
};

bool AllBlack(const Pixels& px) {
    for (std::size_t i = 0; i < px.bgra.size(); i += 4) {
        if (px.bgra[i] | px.bgra[i + 1] | px.bgra[i + 2]) return false;
    }
    return true;
}

// Draws the window's client area with PrintWindow, or copies it from the screen.
bool Capture(HWND hwnd, const WindowSnapshot& w, bool from_screen, Pixels& out) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w.width;
    info.bmiHeader.biHeight = -w.height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap != nullptr && bits != nullptr) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        ok = from_screen ? BitBlt(memory, 0, 0, w.width, w.height, screen, w.screen_x, w.screen_y, SRCCOPY) != 0
                         : PrintWindow(hwnd, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
        GdiFlush();
        if (ok) {
            out.width = w.width;
            out.height = w.height;
            const auto* src = static_cast<const std::uint8_t*>(bits);
            out.bgra.assign(src, src + static_cast<std::size_t>(w.width) * w.height * 4);
            for (std::size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 0xFF;  // GDI leaves alpha 0
        }
        SelectObject(memory, old);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

std::string HresultText(const char* step, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", step, static_cast<unsigned long>(hr));
    return buf;
}

std::string WritePng(const std::filesystem::path& file, const Pixels& px) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    std::string error;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory.p));
        if (FAILED(hr)) error = HresultText("WIC factory", hr);
        if (error.empty() && FAILED(hr = factory->CreateStream(&stream))) error = HresultText("WIC stream", hr);
        if (error.empty() && FAILED(hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) {
            error = HresultText("opening the PNG file", hr);
        }
        if (error.empty() && FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
            error = HresultText("PNG encoder", hr);
        }
        if (error.empty() && FAILED(hr = encoder->Initialize(stream.p, WICBitmapEncoderNoCache))) {
            error = HresultText("PNG encoder init", hr);
        }
        if (error.empty() && FAILED(hr = encoder->CreateNewFrame(&frame, nullptr))) error = HresultText("PNG frame", hr);
        if (error.empty() && FAILED(hr = frame->Initialize(nullptr))) error = HresultText("PNG frame init", hr);
        if (error.empty() && FAILED(hr = frame->SetSize(px.width, px.height))) error = HresultText("PNG size", hr);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (error.empty() && FAILED(hr = frame->SetPixelFormat(&format))) error = HresultText("PNG pixel format", hr);
        if (error.empty() && format != GUID_WICPixelFormat32bppBGRA) error = "PNG encoder refused 32-bit BGRA";
        if (error.empty()) {
            const UINT stride = static_cast<UINT>(px.width) * 4;
            hr = frame->WritePixels(static_cast<UINT>(px.height), stride, static_cast<UINT>(px.bgra.size()),
                                    const_cast<BYTE*>(px.bgra.data()));
            if (FAILED(hr)) error = HresultText("PNG pixels", hr);
        }
        if (error.empty() && FAILED(hr = frame->Commit())) error = HresultText("PNG frame commit", hr);
        if (error.empty() && FAILED(hr = encoder->Commit())) error = HresultText("PNG commit", hr);
    }
    if (uninit) CoUninitialize();
    return error;
}

std::string DefaultName() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char buf[48];
    std::snprintf(buf, sizeof buf, "shot-%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    return buf;
}

// Keeps the newest kMaxScreenshots PNG files in the folder.
void Prune(const std::filesystem::path& folder) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == L".png") files.push_back(entry);
    }
    if (static_cast<int>(files.size()) <= kMaxScreenshots) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) < b.last_write_time(e2);
    });
    for (std::size_t i = 0; i + kMaxScreenshots < files.size(); ++i) std::filesystem::remove(files[i].path(), ec);
}

}  // namespace

ScreenshotResult TakeScreenshot(const std::filesystem::path& folder, std::string_view name) {
    ScreenshotResult r;
    const WindowSnapshot w = ReadGameWindow();
    if (!w.found) {
        r.error = w.reason ? w.reason : "no game window";
        return r;
    }
    if (w.minimized || w.width <= 0 || w.height <= 0) {
        r.error = "the game window is minimized";
        return r;
    }
    const HWND hwnd = reinterpret_cast<HWND>(w.handle);
    Pixels px;
    r.method = "print_window";
    if (!Capture(hwnd, w, false, px) || AllBlack(px)) {
        // PrintWindow draws nothing for some swap chains; the screen copy needs the window uncovered.
        r.method = "screen";
        px = {};
        if (!Capture(hwnd, w, true, px)) {
            r.error = "neither PrintWindow nor a screen copy worked";
            return r;
        }
    }
    r.blank = AllBlack(px);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string base = name.empty() ? DefaultName() : std::string(name);
    const std::filesystem::path file = folder / (std::wstring(base.begin(), base.end()) + L".png");
    if (std::string error = WritePng(file, px); !error.empty()) {
        r.error = error;
        return r;
    }
    Prune(folder);
    r.ok = true;
    const std::u8string u8 = file.u8string();
    r.path.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    r.width = px.width;
    r.height = px.height;
    r.bytes = static_cast<std::uint64_t>(std::filesystem::file_size(file, ec));
    return r;
}

}  // namespace tongs
