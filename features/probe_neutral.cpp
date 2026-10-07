// Lamp colours stay near lamps (see probe_neutral.h)
#include "probe_neutral.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>

namespace ProbeNeutral {
namespace {
using EntryChain::Layer;
using EntryChain::Site;

// LightProbe fields (ctor 0x006B4240; writers 0x006B4720 / 0x006B4B10; blend 0x006B3160 lerps the old and new cubes into the
// bound ones): bound diffuse / specular, the new capture when double-buffered (byte +0x2C97)
constexpr uintptr_t kDiffuse = 0x2C50, kSpecular = 0x2C5C, kDiffNew = 0x2C60, kSpecNew = 0x2C64, kDoubleBuffered = 0x2C97;
// Game texture object: +0x2C kind (1 = cube, 0x00619910), +0x30 the D3D texture (0x00618FB0 / 0x006192E0 lock it)
constexpr uintptr_t kTexKind = 0x2C, kTexD3D = 0x30;

enum Result : int { kDone, kNoTexture, kNotCube, kFormat, kLockFailed, kFault };
struct CubeStats { // POD (filled inside __try)
    uint32_t texels;
    uint32_t size;
    uint32_t levels;
    uintptr_t field;
};

std::mutex g_ctrl;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<float> g_strength{1.0f}, g_night{0.0f};
std::atomic<long> c_diff{0}, c_spec{0}, c_done{0}, c_day{0}, c_fail{0};
std::atomic<int> g_lastBucket{-1};
std::atomic<bool> g_failLogged{false};

// None until the night level passes 0.5 (sunset keeps its colours), smooth to full at 0.9; x strength
int Amount256() {
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    float w = (g_night.load(std::memory_order_relaxed) - 0.5f) / 0.4f;
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    w = w * w * (3.0f - 2.0f * w);
    return static_cast<int>(w * g_strength.load(std::memory_order_relaxed) * 256.0f + 0.5f);
}

// rgb -> lerp(rgb, luma, a/256), alpha kept; Rec.709 weights on the stored 8-bit values (54 + 183 + 19 = 256)
void NeutralRows(uint8_t* bits, int pitch, uint32_t w, uint32_t h, int a) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* p = reinterpret_cast<uint32_t*>(bits + static_cast<intptr_t>(y) * pitch);
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = p[x];
            const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            const int l = (54 * r + 183 * g + 19 * b + 128) >> 8;
            const int r2 = r + (((l - r) * a) >> 8), g2 = g + (((l - g) * a) >> 8), b2 = b + (((l - b) * a) >> 8);
            p[x] = (c & 0xFF000000u) | (static_cast<uint32_t>(r2) << 16) | (static_cast<uint32_t>(g2) << 8) | static_cast<uint32_t>(b2);
        }
    }
}

// The cube the writer just filled (single or double-buffered field), every level and face (POD only, SEH)
int NeutralProbeCube(uint8_t* probe, uintptr_t single, uintptr_t buffered, int a, CubeStats* st) {
    __try {
        const uintptr_t field = *reinterpret_cast<const uint8_t*>(probe + kDoubleBuffered) ? buffered : single;
        st->field = field;
        const uint8_t* tex = *reinterpret_cast<uint8_t* const*>(probe + field);
        if (!tex) return kNoTexture;
        if (*reinterpret_cast<const uint32_t*>(tex + kTexKind) != 1) return kNotCube;
        IDirect3DCubeTexture9* cube = *reinterpret_cast<IDirect3DCubeTexture9* const*>(tex + kTexD3D);
        if (!cube || cube->GetType() != D3DRTYPE_CUBETEXTURE) return kNotCube;
        const DWORD levels = cube->GetLevelCount();
        st->levels = levels;
        for (DWORD lv = 0; lv < levels; lv++) {
            D3DSURFACE_DESC d;
            if (FAILED(cube->GetLevelDesc(lv, &d))) return kLockFailed;
            if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) return kFormat;
            if (lv == 0) st->size = d.Width;
            for (int f = 0; f < 6; f++) {
                D3DLOCKED_RECT lr;
                const D3DCUBEMAP_FACES face = static_cast<D3DCUBEMAP_FACES>(f);
                if (FAILED(cube->LockRect(face, lv, &lr, nullptr, 0))) return kLockFailed; // as the game locks them
                NeutralRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, d.Width, d.Height, a);
                cube->UnlockRect(face, lv);
                st->texels += d.Width * d.Height;
            }
        }
        return kDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kFault;
    }
}

const char* ResultText(int r) {
    switch (r) {
    case kNoTexture: return "no texture";
    case kNotCube: return "not a cube";
    case kFormat: return "format not 8-bit ARGB";
    case kLockFailed: return "LockRect failed";
    case kFault: return "access fault";
    default: return "done";
    }
}

void AfterWrite(uint8_t* probe, uintptr_t single, uintptr_t buffered, const char* what) {
    const int a = Amount256();
    if (a <= 0) {
        c_day.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    CubeStats st{};
    const int r = NeutralProbeCube(probe, single, buffered, a, &st);
    QueryPerformanceCounter(&t1);
    if (r != kDone) {
        c_fail.fetch_add(1, std::memory_order_relaxed);
        if (!g_failLogged.exchange(true))
            LOG_WARNING(std::format("[ProbeNeutral] {} cube of probe {:#x} (+{:#x}) left as the game wrote it: {}", what, reinterpret_cast<uintptr_t>(probe), st.field, ResultText(r)));
        return;
    }
    c_done.fetch_add(1, std::memory_order_relaxed);
    const int bucket = a / 64; // a line per quarter step of the weight (dusk, dawn, setting changes), not per capture
    if (g_lastBucket.exchange(bucket) != bucket) {
        QueryPerformanceFrequency(&fq);
        LOG_INFO(std::format("[ProbeNeutral] Lamp colours out of the sky light at {}% (night {:.2f}): {} cube of probe {:#x} (+{:#x}), {} px x {} levels, {} texels, {:.3f} ms",
                             a * 100 / 256, g_night.load(), what, reinterpret_cast<uintptr_t>(probe), st.field, st.size, st.levels, st.texels,
                             (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart));
    }
}

using FnDiffuse = void(__fastcall*)(uint8_t* probe, void* edx);
using FnSpecular = char(__fastcall*)(uint8_t* probe, void* edx, void* rt);

// 0x006B4720 thiscall(probe), ret (one caller, 0x006B55A8)
void __fastcall DiffuseWriteHook(uint8_t* probe, void* edx) {
    reinterpret_cast<FnDiffuse>(EntryChain::Next(Site::ProbeDiffuseWrite, Layer::ProbeNeutral))(probe, edx);
    c_diff.fetch_add(1, std::memory_order_relaxed);
    AfterWrite(probe, kDiffuse, kDiffNew, "diffuse");
}
// 0x006B4B10 thiscall(probe, rt), ret 4, al (one caller, 0x006B5667): false = nothing was written
char __fastcall SpecularWriteHook(uint8_t* probe, void* edx, void* rt) {
    const char ok = reinterpret_cast<FnSpecular>(EntryChain::Next(Site::ProbeSpecularWrite, Layer::ProbeNeutral))(probe, edx, rt);
    c_spec.fetch_add(1, std::memory_order_relaxed);
    if (ok) AfterWrite(probe, kSpecular, kSpecNew, "specular");
    return ok;
}
} // namespace

bool Install(std::string& error) {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (g_installed) return true;
    using GameAddr::Id;
    std::string why;
    if (!GameAddr::Have({Id::ProbeDiffuseWrite, Id::ProbeSpecularWrite}, &why)) {
        error = "Lamp colours stay near lamps: " + GameAddr::NotAvailable(why);
        return false;
    }
    std::string err;
    if (!EntryChain::Install(Site::ProbeDiffuseWrite, Layer::ProbeNeutral, reinterpret_cast<void*>(&DiffuseWriteHook), &err)) {
        error = "Lamp colours stay near lamps: " + err;
        return false;
    }
    if (!EntryChain::Install(Site::ProbeSpecularWrite, Layer::ProbeNeutral, reinterpret_cast<void*>(&SpecularWriteHook), &err)) {
        EntryChain::Remove(Site::ProbeDiffuseWrite, Layer::ProbeNeutral);
        error = "Lamp colours stay near lamps: " + err;
        return false;
    }
    g_installed = true;
    LOG_INFO(std::format("[ProbeNeutral] Installed on the light probe writers {:#x} (diffuse) and {:#x} (specular)", GameAddr::Get(Id::ProbeDiffuseWrite),
                         GameAddr::Get(Id::ProbeSpecularWrite)));
    return true;
}

void Uninstall() {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (!g_installed) return;
    EntryChain::Remove(Site::ProbeSpecularWrite, Layer::ProbeNeutral);
    EntryChain::Remove(Site::ProbeDiffuseWrite, Layer::ProbeNeutral);
    g_installed = false;
    LOG_INFO(std::format("[ProbeNeutral] Uninstalled (writes: diffuse {}, specular {}; made neutral {}; by day {}; failed {})", c_diff.load(), c_spec.load(), c_done.load(),
                         c_day.load(), c_fail.load()));
}

void Set(bool on, float strength) {
    g_on.store(on, std::memory_order_relaxed);
    g_strength.store(strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength), std::memory_order_relaxed);
}
void SetNightLevel(float level) { g_night.store(level, std::memory_order_relaxed); }

std::string Status() {
    return std::format("probe writes diffuse {} / specular {}, made neutral {}, by day {}, failed {}, weight now {}%", c_diff.load(), c_spec.load(), c_done.load(), c_day.load(),
                       c_fail.load(), Amount256() * 100 / 256);
}
} // namespace ProbeNeutral
