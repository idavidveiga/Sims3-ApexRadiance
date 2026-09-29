// The game's room light maps, as the indoor draws bind them (part of Night Lighting, "Smooth indoor light").
//
// Indoors, stairs, instanced objects and furniture are lit from per-story maps the room solve writes: the room light map
// (A8R8G8B8, e.g. 256x256 over the lot, 4 texels per metre) and, where the lot has high lighting quality, the 4
// directional basis maps (e.g. 64x64, 32x64: 1 texel per metre). Their alpha marks the house plan (VERIFIED on Light Probe
// captures of 29/09: the basis maps' alpha is 255 on exactly the house and 0 outside, where the colour is black), which
// the smooth reads use to average only the texels inside the house (ShaderPatches::BicubicTaps), so the edge no longer
// pulls towards black. Nothing here writes the game's textures: an earlier version padded them on the CPU, and the game
// rewriting them while the camera moved made the light flicker (user, 29/09).
//
// This module only remembers, from the draws of the pixel shaders that read the basis maps (IsBasisPs), which basis maps
// go with which room light map (BasisFor): indoor objects lit by a rig bind only the room light map, and get its basis
// maps from here.
#include "room_map_padding.h"
#include "apex_log.h"
#include "shader_patches.h"
#include <windows.h>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <vector>

namespace {

struct BasisSet {
    IDirect3DTexture9* tex[4] = {}; // +X, -X, +Z, -Z, AddRef'd while kept
    uint32_t lastSeen = 0;          // frame of the last draw that bound them together
};

struct PsInfo {
    std::array<int, 4> dirs{-1, -1, -1, -1}; // sampler per direction (-1: not a readable basis shader)
    uint16_t declared = 0;                    // samplers the shader declares (only those are looked at)
};

bool g_enabled = true;
uint32_t g_frame = 1;
std::unordered_map<IDirect3DTexture9*, BasisSet> g_basisOf;   // room light map (AddRef'd) -> its basis maps
std::unordered_map<IDirect3DPixelShader9*, PsInfo> g_psInfo;  // basis-reading PS -> its basis samplers
std::unordered_map<IDirect3DBaseTexture9*, uint32_t> g_noted; // +X basis map -> frame its draw was last looked at
int g_sets = 0;

constexpr uint32_t kCheckEvery = 30;   // frames between two looks at the draws of one set (per story and lot)
constexpr uint32_t kForgetAfter = 600; // frames without a draw binding a set: released

void Release(BasisSet& s, IDirect3DTexture9* lm) {
    for (auto* t : s.tex)
        if (t) t->Release();
    if (lm) lm->Release();
}

// A MANAGED single-level A8R8G8B8 2D texture (the room light maps and their basis maps)
bool RoomMapLike(IDirect3DBaseTexture9* t) {
    if (!t || t->GetType() != D3DRTYPE_TEXTURE || t->GetLevelCount() != 1) return false;
    D3DSURFACE_DESC d{};
    if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return false;
    return d.Format == D3DFMT_A8R8G8B8 && d.Pool == D3DPOOL_MANAGED && d.Width >= 8 && d.Height >= 8 && d.Width <= 1024 && d.Height <= 1024;
}

} // namespace

namespace RoomMapPadding {

void SetEnabled(bool on) {
    if (g_enabled && !on) Clear();
    g_enabled = on;
}

bool IsBasisPs(const DWORD* t, size_t n) {
    // def cN, 0.8944, 0.4472, 0, -0.8944 (opcode 0x51, 5 operand tokens)
    for (size_t k = 0; k + 5 < n; k++) {
        if ((t[k] & 0xFFFF) != 0x51) continue;
        float f[4];
        std::memcpy(f, &t[k + 2], sizeof f);
        if (std::fabs(f[0] - 0.8944f) < 1e-3f && std::fabs(f[1] - 0.4472f) < 1e-3f && f[2] == 0.0f && std::fabs(f[3] + 0.8944f) < 1e-3f) return true;
    }
    return false;
}

void NoteDraw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    if (!g_enabled || !dev) return;
    auto infoIt = g_psInfo.find(ps);
    if (infoIt == g_psInfo.end()) {
        PsInfo info;
        UINT size = 0;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < 65536) {
            std::vector<DWORD> code(size / 4);
            if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
                if (!ShaderPatches::BasisSamplers(code, info.dirs.data())) info.dirs = {-1, -1, -1, -1};
                for (size_t k = 0; k + 2 < code.size(); k++) // dcl + sampler register token
                    if ((code[k] & 0xFFFF) == 0x001F && (((code[k + 2] >> 28) & 7) | (((code[k + 2] >> 11) & 3) << 3)) == 10 && (code[k + 2] & 0x7FF) < 16)
                        info.declared |= static_cast<uint16_t>(1u << (code[k + 2] & 0x7FF));
            }
        }
        infoIt = g_psInfo.emplace(ps, info).first;
    }
    const auto& dirs = infoIt->second.dirs;
    const uint16_t declared = infoIt->second.declared;
    if (dirs[0] < 0) return;
    // one look per set (story and lot) every kCheckEvery frames, keyed by the +X basis map this draw binds
    {
        IDirect3DBaseTexture9* key = nullptr;
        if (FAILED(dev->GetTexture(static_cast<DWORD>(dirs[0]), &key)) || !key) return;
        uint32_t& last = g_noted[key];
        key->Release(); // only a key: never dereferenced
        if (last && g_frame - last < kCheckEvery) return;
        last = g_frame;
    }
    // the basis maps at the shader's 4 basis samplers; the room light map = the one other room-map-like texture bound
    // to a sampler the shader declares
    BasisSet set{};
    IDirect3DTexture9* lm = nullptr;
    int others = 0;
    for (DWORD s = 0; s < 16; s++) {
        if (!(declared & (1u << s))) continue;
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(dev->GetTexture(s, &t)) || !t) continue;
        int dir = -1;
        for (int k = 0; k < 4; k++)
            if (dirs[k] == static_cast<int>(s)) dir = k;
        if (dir >= 0 && RoomMapLike(t)) set.tex[dir] = static_cast<IDirect3DTexture9*>(t); // keeps the reference
        else if (dir < 0 && RoomMapLike(t)) {
            if (lm) lm->Release();
            lm = static_cast<IDirect3DTexture9*>(t);
            others++;
        } else
            t->Release();
    }
    const bool complete = set.tex[0] && set.tex[1] && set.tex[2] && set.tex[3] && lm && others == 1;
    if (!complete) {
        Release(set, lm);
        return;
    }
    set.lastSeen = g_frame;
    auto it = g_basisOf.find(lm);
    if (it == g_basisOf.end()) {
        g_basisOf.emplace(lm, set); // keeps every reference
        if (++g_sets <= 20)
            LOG_INFO(std::format("[RoomMapPadding] Room light map {:08X}: directional maps {:08X} {:08X} {:08X} {:08X}", reinterpret_cast<uintptr_t>(lm),
                                 reinterpret_cast<uintptr_t>(set.tex[0]), reinterpret_cast<uintptr_t>(set.tex[1]), reinterpret_cast<uintptr_t>(set.tex[2]),
                                 reinterpret_cast<uintptr_t>(set.tex[3])));
    } else {
        Release(it->second, nullptr); // the previous set (possibly other textures now)
        it->second = set;
        lm->Release(); // the key already holds one
    }
}

void OnPresent() {
    g_frame++;
    if (g_noted.size() > 4096) g_noted.clear(); // keys of textures long gone (never dereferenced)
    for (auto it = g_basisOf.begin(); it != g_basisOf.end();) {
        if (g_frame - it->second.lastSeen > kForgetAfter) {
            Release(it->second, it->first);
            it = g_basisOf.erase(it);
        } else
            ++it;
    }
}

bool BasisFor(IDirect3DTexture9* lightMap, IDirect3DTexture9* out[4]) {
    if (!g_enabled) return false;
    const auto it = g_basisOf.find(lightMap);
    if (it == g_basisOf.end()) return false;
    it->second.lastSeen = g_frame; // still in use (objects of this story are drawn)
    for (int k = 0; k < 4; k++) out[k] = it->second.tex[k];
    return true;
}

void Clear() {
    for (auto& [lm, s] : g_basisOf) Release(s, lm);
    g_basisOf.clear();
    g_noted.clear();
    g_psInfo.clear();
}

std::string Status() {
    return std::format("{} | room light maps with directional maps: {}", g_enabled ? "on" : "off", g_basisOf.size());
}

} // namespace RoomMapPadding
