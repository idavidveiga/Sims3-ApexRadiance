#include "logo.h"
#include "logo_data.h"
#include "apex_log.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ApexUi {
namespace {

IDirect3DDevice9* g_device = nullptr;
IDirect3DTexture9* g_tex = nullptr;
bool g_failed = false;

// One mip level from the one above: each texel the average of 2x2, weighted by alpha (straight alpha stays straight)
std::vector<uint32_t> HalfSize(const std::vector<uint32_t>& src, int size) {
    const int half = size / 2;
    std::vector<uint32_t> out(static_cast<size_t>(half) * half);
    for (int y = 0; y < half; y++)
        for (int x = 0; x < half; x++) {
            uint32_t a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                const uint32_t p = src[static_cast<size_t>(y * 2 + k / 2) * size + (x * 2 + k % 2)];
                const uint32_t pa = p >> 24;
                a += pa;
                r += ((p >> 16) & 0xFF) * pa;
                g += ((p >> 8) & 0xFF) * pa;
                b += (p & 0xFF) * pa;
            }
            const uint32_t oa = (a + 2) / 4;
            const uint32_t orr = a ? (r + a / 2) / a : 0, og = a ? (g + a / 2) / a : 0, ob = a ? (b + a / 2) / a : 0;
            out[static_cast<size_t>(y) * half + x] = (oa << 24) | (orr << 16) | (og << 8) | ob;
        }
    return out;
}

bool Make() {
    if (g_tex) return true;
    if (g_failed || !g_device) return false;
    constexpr int n = LogoData::kSize;
    int levels = 0;
    for (int s = n; s >= 1; s /= 2) levels++;
    if (FAILED(g_device->CreateTexture(n, n, static_cast<UINT>(levels), 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr)) || !g_tex) {
        g_tex = nullptr;
        g_failed = true;
        LOG_WARNING("[Menu] Logo: the texture could not be made; the menu shows its plain tile");
        return false;
    }
    std::vector<uint32_t> level(LogoData::kPixels, LogoData::kPixels + n * n);
    int size = n;
    for (int l = 0; l < levels; l++) {
        D3DLOCKED_RECT lr{};
        if (FAILED(g_tex->LockRect(static_cast<UINT>(l), &lr, nullptr, 0))) {
            g_tex->Release();
            g_tex = nullptr;
            g_failed = true;
            LOG_WARNING("[Menu] Logo: the texture could not be filled; the menu shows its plain tile");
            return false;
        }
        for (int y = 0; y < size; y++) std::copy_n(level.data() + static_cast<size_t>(y) * size, size, reinterpret_cast<uint32_t*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch));
        g_tex->UnlockRect(static_cast<UINT>(l));
        if (size > 1) {
            level = HalfSize(level, size);
            size /= 2;
        }
    }
    return true;
}

// The DX9 backend sets linear min / mag filtering but not the mip filter (its state block puts the game's back after)
void LinearMips(const ImDrawList*, const ImDrawCmd*) {
    if (g_device) g_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
}

} // namespace

void SetLogoDevice(IDirect3DDevice9* device) { g_device = device; }

void ReleaseLogo() {
    if (g_tex) g_tex->Release();
    g_tex = nullptr;
    g_device = nullptr;
    g_failed = false;
}

bool DrawLogo(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha) {
    if (!dl || !Make()) return false;
    const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    dl->AddCallback(&LinearMips, nullptr);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(g_tex)), min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, a));
    return true;
}

} // namespace ApexUi
