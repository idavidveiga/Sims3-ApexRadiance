#ifndef APEX_NO_DEV_TOOLS // (a build without the developer tools: ApexFlavorDefines=APEX_NO_DEV_TOOLS)
// Bloom alpha probe (development diagnostic)
//
// Captures the A8R8G8B8 back buffer at PostScene's raw-scene boundary, before the game's first depth-disabled draw.
// The Sims 3 uses scene alpha as a bloom mask on walls / objects / roofs. The probe saves that alpha as an 8-bit
// grayscale PNG plus a text report with distribution statistics. It is intentionally read-only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bloom_alpha_probe.h"
#include "post_scene.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "game_version.h"
#include <windows.h>
#include <wincodec.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace BloomAlphaProbe {
namespace {

std::atomic<bool> g_pending{false};
bool g_registered = false; // render thread only
float g_requestedNight = 0.0f;
std::mutex g_statusLock;
std::string g_status = "ready";

void SetStatus(std::string s) {
    std::lock_guard<std::mutex> lock(g_statusLock);
    g_status = std::move(s);
}

const char* Phase(float night) {
    if (night <= 0.01f) return "Day";
    if (night >= 0.99f) return "Night";
    return "Twilight";
}

const wchar_t* PhaseW(float night) {
    if (night <= 0.01f) return L"Day";
    if (night >= 0.99f) return L"Night";
    return L"Twilight";
}

bool WriteGrayPng(const std::filesystem::path& file, const std::vector<BYTE>& gray, UINT w, UINT h) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat8bppGray;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
         IsEqualGUID(fmt, GUID_WICPixelFormat8bppGray) &&
         SUCCEEDED(frame->WritePixels(h, w, static_cast<UINT>(gray.size()), const_cast<BYTE*>(gray.data()))) &&
         SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(enc->Commit());
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

bool ReadSceneAlpha(IDirect3DDevice9* dev, std::vector<BYTE>& alpha, UINT& w, UINT& h, D3DFORMAT& format, D3DMULTISAMPLE_TYPE& msaa, std::string& error) {
    if (!dev) {
        error = "no D3D device";
        return false;
    }

    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* resolved = nullptr;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        error = "GetBackBuffer failed";
        return false;
    }

    D3DSURFACE_DESC d{};
    if (FAILED(bb->GetDesc(&d))) {
        bb->Release();
        error = "GetDesc failed";
        return false;
    }
    w = d.Width;
    h = d.Height;
    format = d.Format;
    msaa = d.MultiSampleType;

    if (d.Format != D3DFMT_A8R8G8B8) {
        bb->Release();
        error = std::format("back buffer format {} has no readable TS3 bloom alpha (expected A8R8G8B8={})",
                            static_cast<unsigned>(d.Format), static_cast<unsigned>(D3DFMT_A8R8G8B8));
        return false;
    }

    IDirect3DSurface9* src = bb;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
        if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) || !resolved ||
            FAILED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE))) {
            if (resolved) resolved->Release();
            bb->Release();
            error = "could not resolve multisampled back buffer";
            return false;
        }
        src = resolved;
    }

    bool ok = SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys &&
              SUCCEEDED(dev->GetRenderTargetData(src, sys));
    if (!ok) {
        if (sys) sys->Release();
        if (resolved) resolved->Release();
        bb->Release();
        error = "GetRenderTargetData failed";
        return false;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        sys->Release();
        if (resolved) resolved->Release();
        bb->Release();
        error = "LockRect failed";
        return false;
    }

    alpha.resize(static_cast<size_t>(d.Width) * d.Height);
    for (UINT y = 0; y < d.Height; ++y) {
        const BYTE* row = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
        BYTE* out = alpha.data() + static_cast<size_t>(y) * d.Width;
        for (UINT x = 0; x < d.Width; ++x) out[x] = row[4 * x + 3]; // little-endian A8R8G8B8 = B,G,R,A
    }
    sys->UnlockRect();

    sys->Release();
    if (resolved) resolved->Release();
    bb->Release();
    return true;
}

int Percentile(const std::array<uint64_t, 256>& hist, uint64_t total, double p) {
    if (!total) return 0;
    const uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(total)));
    uint64_t seen = 0;
    for (int i = 0; i < 256; ++i) {
        seen += hist[static_cast<size_t>(i)];
        if (seen >= target) return i;
    }
    return 255;
}

void WriteReportAndPng(std::vector<BYTE> alpha, UINT w, UINT h, float night, D3DFORMAT format, D3DMULTISAMPLE_TYPE msaa) {
    const std::filesystem::path dir = std::filesystem::path(ApexPaths::ApexDirectory());
    const std::wstring stem = std::wstring(L"ApexRadiance_BloomAlphaProbe_") + PhaseW(night);
    const std::filesystem::path png = dir / (stem + L".png");
    const std::filesystem::path txt = dir / (stem + L".txt");

    std::array<uint64_t, 256> hist{};
    uint64_t sum = 0;
    for (BYTE a : alpha) {
        ++hist[a];
        sum += a;
    }
    const uint64_t total = alpha.size();
    auto countAbove = [&](int threshold) {
        uint64_t n = 0;
        for (int i = threshold + 1; i < 256; ++i) n += hist[static_cast<size_t>(i)];
        return n;
    };
    const auto pct = [&](uint64_t n) { return total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0; };

    bool txtOk = false;
    {
        std::ofstream out(txt, std::ios::out | std::ios::trunc);
        if (out) {
            out << APEX_PRODUCT_NAME " Bloom Alpha Probe\n";
            out << "Read-only capture of the raw scene alpha at the PostScene boundary, before the game's first depth-disabled draw (normally bloom composite / UI).\n";
            out << "Black in the PNG = alpha 0 (no bloom-mask contribution); white = alpha 255 (maximum mask).\n";
            out << "This measures the final scene alpha mask, not which individual object wrote each pixel. Pair it with Lighting + Bloom Census for draw-family attribution.\n\n";
            out << std::format("Game: {}\n", GetGameVersionName());
            out << std::format("Phase tag: {}\n", Phase(night));
            out << std::format("Night level at request: {:.4f}\n", night);
            out << std::format("Size: {}x{}\n", w, h);
            out << std::format("Back buffer format: {} (A8R8G8B8={})\n", static_cast<unsigned>(format), static_cast<unsigned>(D3DFMT_A8R8G8B8));
            out << std::format("Multisample type: {}\n", static_cast<unsigned>(msaa));
            out << std::format("Pixels: {}\n", total);
            out << std::format("Alpha min/max: {}/{}\n", [&] { for (int i=0;i<256;i++) if (hist[i]) return i; return 0; }(),
                               [&] { for (int i=255;i>=0;i--) if (hist[i]) return i; return 0; }());
            out << std::format("Alpha mean: {:.3f}\n", total ? static_cast<double>(sum) / static_cast<double>(total) : 0.0);
            out << std::format("Percentiles: p50={} p90={} p95={} p99={} p99.9={}\n",
                               Percentile(hist, total, 0.50), Percentile(hist, total, 0.90), Percentile(hist, total, 0.95),
                               Percentile(hist, total, 0.99), Percentile(hist, total, 0.999));
            out << std::format("Non-zero: {} ({:.4f}%)\n", total - hist[0], pct(total - hist[0]));
            for (int t : {8, 16, 32, 64, 96, 128, 160, 192, 224, 240}) {
                const uint64_t n = countAbove(t);
                out << std::format("Alpha > {:3}: {} ({:.4f}%)\n", t, n, pct(n));
            }
            out << "\nHistogram (16-value bins):\n";
            for (int base = 0; base < 256; base += 16) {
                uint64_t n = 0;
                for (int i = base; i < base + 16; ++i) n += hist[static_cast<size_t>(i)];
                out << std::format("{:3}-{:3}: {} ({:.4f}%)\n", base, base + 15, n, pct(n));
            }
            txtOk = true;
        }
    }

    const bool pngOk = WriteGrayPng(png, alpha, w, h);
    if (txtOk && pngOk) {
        SetStatus(std::format("saved {} mask: {} + {}", Phase(night), txt.filename().string(), png.filename().string()));
        LOG_INFO(std::format("[BloomAlphaProbe] Saved {} raw scene alpha: {}x{}, non-zero {:.3f}%, mean {:.3f}",
                             Phase(night), w, h, pct(total - hist[0]), total ? static_cast<double>(sum) / static_cast<double>(total) : 0.0));
    } else {
        SetStatus(std::format("capture finished, but {}{} could not be written", txtOk ? "" : "TXT ", pngOk ? "" : "PNG"));
        LOG_WARNING("[BloomAlphaProbe] Capture completed but one or more output files could not be written");
    }
}

void CaptureAtSceneEnd(IDirect3DDevice9* dev) {
    if (!g_pending.exchange(false, std::memory_order_acq_rel)) return;

    const float night = g_requestedNight;
    std::vector<BYTE> alpha;
    UINT w = 0, h = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    D3DMULTISAMPLE_TYPE msaa = D3DMULTISAMPLE_NONE;
    std::string error;
    SetStatus(std::format("reading {} scene alpha...", Phase(night)));

    if (!ReadSceneAlpha(dev, alpha, w, h, fmt, msaa, error)) {
        SetStatus("capture failed: " + error);
        LOG_WARNING("[BloomAlphaProbe] " + error);
        return;
    }

    SetStatus(std::format("captured {} mask; writing PNG/TXT...", Phase(night)));
    // (07/10, players' Runtime Error: started and run under HookGuard::StartDetached, so neither can end the game)
    HookGuard::StartDetached("BloomAlphaProbe: report writer", [alpha = std::move(alpha), w, h, night, fmt, msaa]() mutable {
        WriteReportAndPng(std::move(alpha), w, h, night, fmt, msaa);
    });
}

} // namespace

void Request(float nightLevel) {
    if (g_pending.load(std::memory_order_acquire)) return;
    g_requestedNight = std::clamp(nightLevel, 0.0f, 1.0f);
    if (!g_registered) {
        // Run before Apex's own post-scene effects; the game bloom composite has not drawn yet at this boundary.
        PostScene::Add(0, CaptureAtSceneEnd);
        g_registered = true;
    }
    g_pending.store(true, std::memory_order_release);
    SetStatus(std::format("waiting for {} scene end...", Phase(g_requestedNight)));
    LOG_INFO(std::format("[BloomAlphaProbe] Armed at night level {:.4f} ({})", g_requestedNight, Phase(g_requestedNight)));
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_statusLock);
    return g_status;
}

} // namespace BloomAlphaProbe
#else
// Developer tools left out of this build
#include "bloom_alpha_probe.h"
namespace BloomAlphaProbe {
void Request(float) {}
std::string Status() { return "not in this build"; }
} // namespace BloomAlphaProbe
#endif
