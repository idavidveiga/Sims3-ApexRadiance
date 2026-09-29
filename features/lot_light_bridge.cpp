// Lot light bridge (part of Night Lighting)
//
// Why lot grass has a hard edge next to street lamps (measured with light_probe.cpp):
//  - World grass: the terrain chunk shader adds tex2D(terrainLightMap, uv) * c7.x (s8), where the terrain light map is the
//    world's baked lamp "stamp" (wide diffuse circles). uv = (worldXZ - chunkCenter) / 256 + 0.5.
//  - Lot grass: drawn by the lot terrain passes. Its light pass (modulate2x) adds tex2D(lotLightMap, uv) * c3.x (s1). The
//    lot light map is solved on the CPU (FUN_006be020): street lamps fall off with the squared distance from the lamp head,
//    so they arrive very faint. The two formulas meet at the lot border.
//  - The lot light-pass vertex shader already outputs the terrain light map uv in TEXCOORD1 and gets the chunk center in
//    c15 (xz).
// Fix: when the game draws that exact lot light pass, draw it with a copy of the shader that also samples the terrain light
// map of the same chunk (recorded from the world chunk draws: s8 texture, key = world translation c8.w / c10.w) and uses
// max(lot light, terrain light). Both sides of the border then show the same street-lamp light.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "lot_light_bridge.h"
#include "game_addresses.h"
#include "shader_ids.h"
#include "roof_ps_hlsl.h"
#include "water_lamps_hlsl.h"
#include "roof_snow_lamps_hlsl.h"
#include "wall_lamp_table.h"
#include "floor_atlas_table.h"
#include "shader_patches.h"
#include "lightmap_smooth.h"
#include "light_probe.h"
#include "rig_tracker.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "shader_cache.h"
#include "build_flavor.h"
#include "apex_paths.h"
#include "apex_log.h"
#include "apex_version.h"
#include <windows.h>
#include <d3dcommon.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kHookName = "LotLightBridge";

//
// Soft lot edges (28/09, research\borda2 + borda3): the lot map saturates at 1.0 within ~2.5 m of a street lamp while
// the terrain stamp outside the lot peaks at ~0.7-0.9, so a lamp near a lot edge left a step at the edge. Within the
// band c30.x = 1/band metres of the lot rectangle, the lamp term blends from max(lot, terrain) to the terrain term, which
// is exactly what the world grass shows on the other side. Lot-local position = affine map of the terrain uv (c28, c29,
// built on the CPU from VS c14/c15 and the lot matrix VS c8/c10); lot size W x D (tiles = metres) from room 0 of the
// lot (+0xC0/+0xC4). c30 = (0, 1) turns it off (w = 1 everywhere): unmatched lot or option off.
//   c28 = (dLx/du, dLx/dv, Lx0, W)   c29 = (dLz/du, dLz/dv, Lz0, D)   c30 = (1/band, bias, 0, 0)
const char* kReplacementHlsl = R"(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 cLotX : register(c28);
float4 cLotZ : register(c29);
float4 cEdge : register(c30);
samplerCUBE sSky : register(s0);
sampler2D sLot : register(s1);
sampler2D sTerrain : register(s2);
sampler2D sShadow : register(s5);
struct PSIn {
    float4 shadowPos : TEXCOORD2;
    float3 normal : TEXCOORD4;
    float2 lotUv : TEXCOORD5;
    float3 terrainUv : TEXCOORD1;
};
float4 main(PSIn i) : COLOR0 {
    float4 p = float4(i.shadowPos.xy - 0.5 * c2.y, i.shadowPos.zw);
    float4 s;
    s.y = tex2Dproj(sShadow, p + c2.yzww).x;
    s.z = tex2Dproj(sShadow, p + c2.zyww).x;
    s.x = tex2Dproj(sShadow, p).x;
    s.w = tex2Dproj(sShadow, p + c2.yyww).x;
    float avg = dot(s, 0.25);
    float2 d = i.shadowPos.xy - 0.5;
    float edge = saturate(max(abs(d.x), abs(d.y)) * 8 - 3);
    float sun = lerp(avg, 1, edge) * saturate(dot(i.normal, c1.xyz));
    float3 terrain = tex2D(sTerrain, i.terrainUv.xy).rgb;
    float3 uv1 = float3(i.terrainUv.xy, 1);
    float2 lp = float2(dot(uv1, cLotX.xyz), dot(uv1, cLotZ.xyz)); // lot-local metres
    float2 e = min(lp, float2(cLotX.w, cLotZ.w) - lp);             // distance to the nearer edge on each axis
    float w = saturate(min(e.x, e.y) * cEdge.x + cEdge.y);
    w = w * w * (3 - 2 * w);
    float3 lamps = lerp(terrain, max(tex2D(sLot, i.lotUv).rgb, terrain), w) * c3.x;
    float3 col = sun * c0.rgb + lamps;
    col = texCUBE(sSky, i.normal).rgb * c4.x + col;
    return float4(col * 0.5, 0);
}
)";

// Instanced outdoor objects (fences, shrubs): the vertex shader sums sun + the 3 rig lamps into TEXCOORD2 and this pixel
// shader multiplies that sum by the sun/moon shadow, so at night lamp light vanishes wherever the moon shadow falls
// (e.g. the side of a hedge or planter wall). Replacement: identical, but the shadow fades to 1 by c3.x (night level).
const char* kObjectRigHlsl = R"(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c3 : register(c3);
samplerCUBE sSky : register(s0);
sampler2D sTex : register(s1);
sampler2D sShadow : register(s5);
struct PSIn {
    float4 fog : COLOR0;
    float3 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float3 t2 : TEXCOORD2;
    float4 t4 : TEXCOORD4;
    float t5 : TEXCOORD5;
};
float4 main(PSIn i) : COLOR0 {
    float4 p = float4(i.t4.xy - 0.5 * c0.y, i.t4.zw);
    float4 a = float4(p.x + c0.y, p.y + c0.z, p.z + c0.w, p.w + c0.w);
    float4 b = float4(p.x + c0.z, p.y + c0.y, p.z + c0.w, p.w + c0.w);
    float4 d = float4(p.x + c0.y, p.y + c0.y, p.z + c0.w, p.w + c0.w);
    float4 s = float4(tex2Dproj(sShadow, p).x, tex2Dproj(sShadow, a).x, tex2Dproj(sShadow, b).x, tex2Dproj(sShadow, d).x);
    float sh = lerp(dot(s, 0.25), 1, i.t5);
    sh = lerp(sh, 1, c3.x);
    float3 light = (i.t2 * sh + texCUBE(sSky, i.t0).rgb * c1.w) * i.t1.z;
    float4 tex = tex2D(sTex, i.t1.xy);
    float3 col = lerp(tex.rgb * light, i.fog.rgb, i.fog.w);
    return float4(col, i.t1.w - tex.a);
}
)";

// The five replacement shaders are compiled at start-up on a background thread (framework/shader_cache.h), with the
// options they always had (entry "main", flags 0); the draw hooks only create the shader objects at their first use.
// Before 2026-09-28 each was compiled with D3DCompile inside the first draw that needed it: a one-time hitch on the render
// thread (research\perf2\plan.md, item 7).
ShaderCache::Id AddLotShader(const char* tag, const char* hlsl, const char* target, int priority) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = hlsl;
    d.sourceName = "lot_light_bridge";
    d.entry = "main";
    d.target = target;
    d.flags = 0;
    d.priority = priority;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kReplacementPsId = AddLotShader("NightLighting lot light pass", kReplacementHlsl, "ps_3_0", 0);
const ShaderCache::Id kObjectRigPsId = AddLotShader("NightLighting object rig (moon shadow)", kObjectRigHlsl, "ps_2_0", 0);
const ShaderCache::Id kRoofPsId = AddLotShader("NightLighting roofs", kRoofHlsl, "ps_3_0", 0);
const ShaderCache::Id kWaterPsId = AddLotShader("NightLighting lake water", kWaterLampsHlsl, "ps_3_0", 0);
const ShaderCache::Id kRoofSnowPsId = AddLotShader("NightLighting snowy roofs", kRoofSnowLampsHlsl, "ps_3_0", 1);

enum class PsClass : uint8_t { Unknown, Other, WorldCandidate, LotLight, ObjectRig, Roof, Lake, LotLightSnow, RoofSnow, WallGain, FloorAtlas };

std::atomic<bool> g_enabled{false};
bool g_hooksRegistered = false;
bool g_inOwnCall = false;
IDirect3DPixelShader9* g_curPs = nullptr;
PsClass g_curClass = PsClass::Other;
std::unordered_map<IDirect3DPixelShader9*, PsClass> g_classCache;
IDirect3DPixelShader9* g_replacementPs = nullptr;
IDirect3DPixelShader9* g_objectPs = nullptr;
bool g_objectCompileTried = false;
std::atomic<bool> g_objectFix{false};
std::atomic<float> g_night{0.0f};
std::atomic<int> g_objectDrawn{0};
bool g_compileTried = false;
std::string g_status = "Off";

struct ChunkTex {
    IDirect3DBaseTexture9* tex = nullptr; // AddRef'd
};
std::map<std::pair<int, int>, ChunkTex> g_chunks; // key: chunk center (x, z) rounded
std::atomic<int> g_worldSeen{0}, g_lotDrawn{0}, g_lotMissing{0};

std::pair<int, int> Key(float x, float z) { return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(z))}; }

// The smoothed version of a chunk light map when it is ready (lightmap_smooth.cpp), else the game's own.
IDirect3DBaseTexture9* ChunkTexture(const std::pair<int, int>& key, IDirect3DBaseTexture9* original) {
    if (IDirect3DTexture9* s = LightmapSmooth::Find(key)) return s;
    return original;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// ---- Soft lot edges: the rectangle of every loaded lot, for the lot pass feather (kReplacementHlsl c28..c30). ----
// Walk (render thread, Present): *(0x011D1860)+0x1C0 = lightMgr; +0xD4 light update tree (buckets +0x58, count +0x5C,
// node +8 tracker, next +0x10); tracker+0x6A0 + level*0x1A4 = tree level, whose +0 is the story's manager; manager
// +0x90/+0x94 lot id, room hash +0x234 / +0x238 (node: +0 room id, +0x10 room, +0x80 next). Room 0:
//  - +0xF8 -> 4x4 lot->world matrix, row vectors: translation m[12], m[14]; the lot pass VS has the same matrix in
//    c8 = (m0, m4, m8, m12), c10 = (m2, m6, m10, m14) (LightDiag "matriz[+0xF8]" of lot 09080020A1D28860 = VS c8/c10
//    of the lot pass in research\borda3, rotation and translation);
//  - +0xC0 / +0xC4 = tile extent (x, z) of the room. For room 0 the rebuild FUN_006a2740 copies them from the manager's
//    tile grid size +0x264 / +0x268 (the room-id grid +0x260 bounds-checked with them everywhere, and "LotSizeParameters"
//    = size / 64 in FUN_006a4c10), FUN_0069efc0 walks tiles [0, C0) x [0, C4), and FUN_006c6ab0 gathers world lights
//    at the lot centre (C0 / 2, 0, C4 / 2) through +0xF8. So the lot covers lot-local [0, C0] x [0, C4] metres.
struct LotRect {
    float tx, tz;         // lot origin (world x, z)
    float m0, m8;         // first row of the rotation (VS c8.x, c8.z) for the match
    float w, d;           // size in metres along lot-local x and z
    uint32_t lotLo, lotHi;
};
std::vector<LotRect> g_lotRects;
std::atomic<bool> g_softEdges{true};
constexpr float kEdgeBand = 3.0f; // metres of feather inside the lot edge
std::atomic<int> g_edgeMatched{0}, g_edgeUnmatched{0};
bool g_lotRectMiss = false; // a lot pass found no rectangle: refresh the table at the next Present
int g_lotRectFrame = 0;
LotRect g_lastEdgeRect{};        // the last lot the feather was applied to (status line)
bool g_haveLastEdgeRect = false;

// Story order: level 0 first (the terrain story, which draws the lot ground); every story has the same matrix and size.
constexpr int kLotLevels[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -3, -4};

// SEH only (no C++ objects): fills out[0..max), returns the count or -1 on a fault / no world.
int ReadLotRects(LotRect* out, int max) {
    int n = 0;
    const uintptr_t rootPtr = GameAddr::Get(GameAddr::Id::RootPtr); // 0x011D1860 on Steam, found by signature elsewhere
    if (!rootPtr) return -1;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(rootPtr);
        const uintptr_t lightMgr = root ? *reinterpret_cast<const uintptr_t*>(root + 0x1C0) : 0;
        if (!lightMgr) return -1;
        const uintptr_t tree = *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD4);
        if (!tree) return -1;
        const uintptr_t buckets = *reinterpret_cast<const uintptr_t*>(tree + 0x58);
        const uint32_t bucketCount = *reinterpret_cast<const uint32_t*>(tree + 0x5C);
        if (!buckets || !bucketCount || bucketCount >= (1u << 20)) return -1;
        const uintptr_t endNode = *reinterpret_cast<const uintptr_t*>(buckets + bucketCount * 4);
        uintptr_t slot = buckets;
        uintptr_t node = *reinterpret_cast<const uintptr_t*>(slot);
        int guard = 0;
        while (node == 0 && guard++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        guard = 0;
        while (node && node != endNode && guard++ < 100000 && n < max) {
            const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(node + 8);
            for (int li = 0; tracker && li < 12; li++) {
                const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(tracker + 0x6A0 + static_cast<intptr_t>(kLotLevels[li]) * 0x1A4);
                if (!mgr || *reinterpret_cast<const uintptr_t*>(mgr) != lightMgr) continue;
                const uintptr_t rb = *reinterpret_cast<const uintptr_t*>(mgr + 0x234);
                const uint32_t rc = *reinterpret_cast<const uint32_t*>(mgr + 0x238);
                uintptr_t room0 = 0;
                for (uint32_t b = 0; rb && rc < 100000 && b < rc && !room0; b++) {
                    int g2 = 0;
                    for (uintptr_t rn = *reinterpret_cast<const uintptr_t*>(rb + b * 4); rn && g2++ < 10000; rn = *reinterpret_cast<const uintptr_t*>(rn + 0x80))
                        if (*reinterpret_cast<const int*>(rn) == 0) {
                            room0 = *reinterpret_cast<const uintptr_t*>(rn + 0x10);
                            break;
                        }
                }
                if (!room0) continue;
                const uint32_t w = *reinterpret_cast<const uint32_t*>(room0 + 0xC0), d = *reinterpret_cast<const uint32_t*>(room0 + 0xC4);
                const float* m = *reinterpret_cast<const float* const*>(room0 + 0xF8);
                if (!m || w == 0 || d == 0 || w > 256 || d > 256) continue; // room not rebuilt yet, or not a lot grid
                LotRect& r = out[n];
                r.tx = m[12];
                r.tz = m[14];
                r.m0 = m[0];
                r.m8 = m[8];
                r.w = static_cast<float>(w);
                r.d = static_cast<float>(d);
                r.lotLo = *reinterpret_cast<const uint32_t*>(mgr + 0x90);
                r.lotHi = *reinterpret_cast<const uint32_t*>(mgr + 0x94);
                if (std::isfinite(r.tx) && std::isfinite(r.tz) && std::isfinite(r.m0) && std::isfinite(r.m8)) n++;
                break; // one story per lot is enough
            }
            node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            int g3 = 0;
            while (node == 0 && g3++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

void RefreshLotRects() {
    static LotRect buf[1024];
    const int n = ReadLotRects(buf, 1024);
    if (n < 0) {
        g_lotRects.clear();
        return;
    }
    g_lotRects.assign(buf, buf + n);
}

// The rectangle of the lot the current lot pass draws, matched by its matrix (VS c8, c10). nullptr = unknown lot.
const LotRect* FindLotRect(const float c8[4], const float c10[4]) {
    for (const LotRect& r : g_lotRects)
        if (std::fabs(r.tx - c8[3]) < 0.05f && std::fabs(r.tz - c10[3]) < 0.05f && std::fabs(r.m0 - c8[0]) < 2e-3f && std::fabs(r.m8 - c8[2]) < 2e-3f) return &r;
    return nullptr;
}

// PS c28..c30 of kReplacementHlsl. k = the VS c14 the draw runs with (terrain uv = (world.xz - c15.xz) * k.xy + k.zw),
// c15 = chunk centre, c8 / c10 = lot matrix rows (world.x = c8.x lx + c8.z lz + c8.w, world.z = c10.x lx + c10.z lz +
// c10.w). Inverts both into lot-local = A * uv + b (double precision on the CPU; the shader only does two dot products).
bool LotEdgeConstants(const float k[4], const float c15[4], const float c8[4], const float c10[4], const LotRect* r, float out[12]) {
    const float off[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}; // w = 1 everywhere: the plain max()
    std::memcpy(out, off, sizeof(off));
    if (!r || !g_softEdges.load(std::memory_order_relaxed)) return false;
    const double a = c8[0], b = c8[2], c = c10[0], d = c10[2];
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-4 || std::fabs(k[0]) < 1e-9f || std::fabs(k[1]) < 1e-9f) return false;
    const double sx = 1.0 / k[0], sz = 1.0 / k[1];
    const double ox = c15[0] - k[2] * sx - c8[3]; // world.x - tx = uv.x * sx + ox
    const double oz = c15[2] - k[3] * sz - c10[3];
    out[0] = static_cast<float>(d * sx / det);
    out[1] = static_cast<float>(-b * sz / det);
    out[2] = static_cast<float>((d * ox - b * oz) / det);
    out[3] = r->w;
    out[4] = static_cast<float>(-c * sx / det);
    out[5] = static_cast<float>(a * sz / det);
    out[6] = static_cast<float>((-c * ox + a * oz) / det);
    out[7] = r->d;
    out[8] = 1.0f / kEdgeBand;
    out[9] = 0.0f;
    return true;
}

// ---- Outdoor walls. Their lamp light is only the game's baked wall atlas (room solve, lamps x k2 = 0.075), much
// dimmer than the rig lamps objects get, so walls look darker than the objects in front of them (user, 25/09). The
// pixel shaders of the game's ExteriorWall technique (Shaders_Win32.precomp) add it with "texld rA, vT, s2" then
// "mad rB.xyz, rA, cK.x, rC"; K differs per variant (in others c3.x is the bloom threshold). wall_lamp_table.h lists
// every ExteriorWall pixel shader by size + FNV-1a (32-bit, over DWORDs) with its K, generated offline; a draw with
// one of them gets cK.x multiplied by "Forca nas paredes". No shader is changed; interior walls are never in the table.
std::unordered_map<IDirect3DPixelShader9*, DWORD> g_wallConst;
std::atomic<float> g_wallGain{1.0f};

int WallLampConst(const DWORD* t, size_t bytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes / 4; i++) h = (h ^ t[i]) * 16777619u;
    for (const WallLampEntry& e : kWallLampTable)
        if (e.size == bytes && e.hash == h) return static_cast<int>(e.constant);
    return -1;
}

// Outdoor floors lit only by their baked floor map (floor_atlas_table.h; nearly black in summer, LightProbe-m61).
bool IsFloorAtlasPs(const DWORD* t, size_t bytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes / 4; i++) h = (h ^ t[i]) * 16777619u;
    for (const FloorAtlasEntry& e : kFloorAtlasTable)
        if (e.size == bytes && e.hash == h) return true;
    return false;
}

// Every shader we classify is pinned (AddRef) until Shutdown: all caches here are keyed by the shader pointer, and a
// released shader's address could otherwise be reused by a new one that would then get the old one's class or patched
// copy (review 25/09).
std::vector<IUnknown*> g_pinned;

PsClass Classify(IDirect3DPixelShader9* ps) {
    if (!ps) return PsClass::Other;
    auto it = g_classCache.find(ps);
    if (it != g_classCache.end()) return it->second;
    PsClass c = PsClass::Other;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < 65536) {
        std::vector<BYTE> code(size);
        if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
            if (IsShader(kLotLightPs, code.data(), size)) c = PsClass::LotLight;
            else if (IsShader(kObjectRigPs, code.data(), size)) c = PsClass::ObjectRig;
            else if (IsShader(kRoofPs, code.data(), size)) c = PsClass::Roof;
            else if (IsShader(kLakePs, code.data(), size)) c = PsClass::Lake;
            else if (IsShader(kSnowLotPs, code.data(), size)) c = PsClass::LotLightSnow;
            else if (IsShader(kRoofSnowPs, code.data(), size)) c = PsClass::RoofSnow;
            else if (const int k = WallLampConst(reinterpret_cast<const DWORD*>(code.data()), size); k >= 0) {
                g_wallConst[ps] = static_cast<DWORD>(k);
                c = PsClass::WallGain;
            }
            else if (IsFloorAtlasPs(reinterpret_cast<const DWORD*>(code.data()), size)) c = PsClass::FloorAtlas;
            else {
                // Declares a sampler s6 or higher (terrain shaders; dcl token 0x0200001F followed by 0x90000000 | type, then register token s8)
                const auto* t = reinterpret_cast<const DWORD*>(code.data());
                const size_t n = size / 4;
                for (size_t k = 0; k + 2 < n; k++)
                    // register token: number in bits 0-10, type = bits 28-30 | bits 11-12 << 3 (sampler = 10)
                    if ((t[k] & 0xFFFF) == 0x001F && (t[k + 2] & 0x7FF) >= 6 && (((t[k + 2] >> 28) & 7) | (((t[k + 2] >> 11) & 3) << 3)) == 10) {
                        c = PsClass::WorldCandidate;
                        break;
                    }
            }
        }
    }
    g_classCache[ps] = c;
    ps->AddRef();
    g_pinned.push_back(ps);
    return c;
}

// Creates the pixel shader from its precompiled bytecode (shader_cache.h; compiled at start-up off the render thread).
// Returns an error text, empty on success.
std::string CompilePs(IDirect3DDevice9* dev, ShaderCache::Id id, IDirect3DPixelShader9** out) {
    std::string msg;
    switch (ShaderCache::CreatePixelShader(dev, id, out, &msg)) {
    case ShaderCache::Result::Ok:
        return {};
    case ShaderCache::Result::CompileFailed:
        return std::format("compile failed: {}", msg.empty() ? std::string("?") : msg);
    case ShaderCache::Result::CreateFailed:
        break;
    }
    *out = nullptr;
    return "could not create the shader";
}

// ---- Roofs: the game's roof shader has no lamp light at all (sun/moon + sky only). ----
std::atomic<bool> g_roofFix{false};
std::atomic<float> g_roofStrength{1.0f};
IDirect3DPixelShader9* g_roofPs = nullptr;
bool g_roofCompileTried = false;
IDirect3DVertexShader9* g_curVs = nullptr;
bool g_stateUnknown = true;               // the bound shaders were not seen by our Set*Shader hooks (see OnDraw)
std::atomic<bool> g_hookFailed{false};    // an exception escaped a hook: everything off (HookFailed)
bool g_curVsIsRoof = false;
bool g_curVsIsLake = false;
bool g_curVsIsSnowLot = false;
bool g_curVsIsRoad = false;
DWORD g_curRoadMap = 16;                                              // VS constant with the road's terrain uv mapping
std::unordered_map<IDirect3DVertexShader9*, DWORD> g_roadMapConst;    // per road vertex shader
bool g_curVsIsFloor = false;
bool g_curVsIsSnowFloor = false; // snow lying on lot floor tiles (LightProbe-m69, m71)
int g_curSnowFloorTc = 7;
std::unordered_map<IDirect3DVertexShader9*, int> g_snowFloorTc; // per snow-floor VS: where it puts world xz / 2
bool g_curVsIsSnowCover = false;
bool g_curVsIsSnowRelief = false;
bool g_curVsIsFoliage = false;
bool g_curVsIsInstanced = false; // fence rails/posts, railings, stairs (SceneModelArray)
struct FoliageVs {
    std::vector<DWORD> code; // patched (ShaderPatches::PatchFoliageVs)
    IDirect3DVertexShader9* vs = nullptr;
    bool tried = false;
    int worldK = -1; // objects: first VS constant of the world triple (c[K..K+2].w = the object position)
    int vertexLight = -1; // objects: first colour constant of the rig's 4 vertex lights (COLOR0), -1 if not found
};
std::unordered_map<IDirect3DVertexShader9*, FoliageVs> g_foliageVs;
std::unordered_map<IDirect3DVertexShader9*, FoliageVs> g_objectVs; // objects lit by a rig: + world xzy in TEXCOORD8 (ShaderPatches::PatchObjectLampVs)
bool g_curVsIsObject = false;
std::unordered_map<IDirect3DVertexShader9*, uint8_t> g_vsCache; // 0 other, 1 roof, 2 lake, 3 snow lot, 4 road, 5 floor, 6 foliage, 7 fence/stairs, 8 snow on objects, 9 snow with relief (stair tops), 10 object lit by a rig
std::atomic<int> g_roofDrawn{0};
float g_cam[3] = {};
float g_lampData[33][4] = {}; // 16 x pos+radius, 16 x colour, params
int g_lampCount = 0;
int g_lampFrame = 0;

uint8_t ClassifyVs(IDirect3DVertexShader9* vs) {
    if (!vs) return 0;
    auto it = g_vsCache.find(vs);
    if (it != g_vsCache.end()) return it->second;
    uint8_t cls = 0;
    UINT size = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
        std::vector<BYTE> code(size);
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) {
            auto is = [&](const ShaderId& id) { return IsShader(id, code.data(), size); };
            if (is(kRoofVs)) cls = 1;
            else if (is(kLakeVs)) cls = 2;
            else if (is(kSnowLotVs)) cls = 3;
            else if (is(kFloorVs)) cls = 5;
            else {
                std::vector<DWORD> t(size / 4);
                std::memcpy(t.data(), code.data(), t.size() * 4);
                DWORD mapConst = 0;
                if (ShaderPatches::IsRoadVs(t, mapConst)) {
                    g_roadMapConst[vs] = mapConst; // c16 in winter, c14 in summer
                    cls = 4;
                } else if (ShaderPatches::IsInstancedStructureVs(t))
                    cls = 7;
                else if (ShaderPatches::IsSnowCoverVs(t))
                    cls = 8;
                else if (ShaderPatches::IsSnowReliefVs(t))
                    cls = 9;
                else if (ShaderPatches::IsFloorVs(t)) // floor variants, e.g. the curved pool edge (LightProbe-m66)
                    cls = 5;
                else if (ShaderPatches::PatchFoliageVs(t)) {
                    g_foliageVs[vs].code = std::move(t);
                    cls = 6;
                } else {
                    std::vector<DWORD> o = t;
                    int tc = 7, wk = -1, vl = -1;
                    if (ShaderPatches::PatchObjectLampVs(o, true, nullptr, &wk, &vl)) {
                        g_objectVs[vs].code = std::move(o);
                        g_objectVs[vs].worldK = wk;
                        g_objectVs[vs].vertexLight = vl;
                        cls = 10;
                    } else if (ShaderPatches::IsSnowFloorVs(t, tc)) {
                        // last: the TEXCOORD0.zw shape is shared by terrain and lot passes, which the draw dispatch
                        // handles first (DrawSnowFloor only runs when no pixel-shader class claimed the draw)
                        g_snowFloorTc[vs] = tc;
                        cls = 11;
                    }
                }
            }
        }
    }
    g_vsCache[vs] = cls;
    vs->AddRef();
    g_pinned.push_back(vs);
    return cls;
}

// Light enumeration (FUN_006acf70, stdcall(visitor), visitor vtable[0] = thiscall(visitor, Light*))
std::vector<uintptr_t> g_enumLights;
void __fastcall EnumVisit(void*, void*, uintptr_t light) {
    if (g_enumLights.size() < 100000) g_enumLights.push_back(light);
}
void* g_enumVtbl[1] = {reinterpret_cast<void*>(&EnumVisit)};
struct EnumVisitor {
    void** vtbl;
} g_enumVisitor{g_enumVtbl};
bool g_enumChecked = false, g_enumOk = false;

uintptr_t g_enumFn = 0; // 0x006ACF70 on Steam, found by signature on other builds (game_addresses.h)

bool EnumerateLights() {
    if (!g_enumChecked && GameAddr::Resolved()) {
        g_enumChecked = true;
        g_enumFn = GameAddr::Get(GameAddr::Id::EnumLights);
        static const BYTE expect[] = {0xE8, 0x2B, 0x36, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x04, 0x51, 0x68, 0x40, 0xCF, 0x6A, 0x00}; // Steam (checked on Steam)
        g_enumOk = g_enumFn && (!GameAddr::IsFixed() || std::memcmp(reinterpret_cast<const void*>(g_enumFn), expect, sizeof(expect)) == 0);
    }
    if (!g_enumOk) return false;
    g_enumLights.clear();
    __try {
        reinterpret_cast<void(__stdcall*)(void*)>(g_enumFn)(&g_enumVisitor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLamp(uintptr_t L, float out[8]) {
    __try {
        const BYTE f = *reinterpret_cast<const BYTE*>(L + 0x100);
        if (!(f & 0x01) || !(f & 0x20)) return false; // alive and lit
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        const int room = *reinterpret_cast<const int*>(L + 0x08);
        if (type != 0xB && !((f & 0x04) && room == 0)) return false; // world lamp or outdoor lot lamp
        const float* head = reinterpret_cast<const float*>(L + 0x120);
        const float* bounds = reinterpret_cast<const float*>(L + 0x134);
        const float* col = reinterpret_cast<const float*>(L + 0xF0);
        const float inten = *reinterpret_cast<const float*>(L + 0x10);
        const float fade = *reinterpret_cast<const float*>(L + 0x20);
        // Visual reach of the lamp's light pool: from its range value +0x130 (97, 40, 100...), about 7-12 m. The light
        // bounds (+0x134) are much larger (~50 m) and made roofs and water blow out to white.
        (void)bounds;
        const float range = *reinterpret_cast<const float*>(L + 0x130);
        if (!(range > 0.01f)) return false;
        float radius = std::sqrt(range) * 1.2f;
        radius = radius < 2.0f ? 2.0f : (radius > 25.0f ? 25.0f : radius);
        out[0] = head[0]; out[1] = head[1]; out[2] = head[2]; out[3] = radius;
        out[4] = col[0] * inten * fade; out[5] = col[1] * inten * fade; out[6] = col[2] * inten * fade; out[7] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Picks the 16 lit outdoor lamps nearest to the camera.
std::vector<std::array<float, 8>> g_allLamps; // every lit outdoor lamp: pos, radius, colour

// Refreshes the list of lit outdoor lamps (lamps rarely change, so every 20 frames is plenty).
void UpdateLampList() {
    if (!EnumerateLights()) return;
    g_allLamps.clear();
    for (uintptr_t L : g_enumLights) {
        std::array<float, 8> v;
        if (ReadLamp(L, v.data())) g_allLamps.push_back(v);
    }
    g_lampCount = static_cast<int>(g_allLamps.size());
}

// ---- Changes of outdoor lot lamps (Build mode, lamps switching by themselves) and the snapshot of what the terrain
// bake can take. The terrain light has to be rebuilt for changes of lamps IN the bake; the game does it only when the
// lot is reloaded. What the bake draws per lamp (docs/engine/terrain-and-light-bake.md 4.2): every light accepted by the
// visitor (street-lamp class, type 0xB; with Apex also outdoor lot lamps of type 3..6 that are enabled and lit,
// TerrainLightTest) whose rect +0x134 overlaps the chunk, at its position (vfunc+0x24, +0x120), with colour +0xF0 and
// weight range +0x130 x intensity +0x10 x 0.2. The fade +0x20 and the effective colour +0xE0 are NOT read by the bake.
// So a change counts only when:
//  - it changes the bake: the lamp enters or leaves it (lit flag, enabled flag, intensity to or from 0), moves by more
//    than 5 cm, or its light (colour x intensity x range) changes by more than 5 % in a channel. Lamps the bake never
//    takes (window lights 7/8, type 9, disabled or unlit lamps) never count. 29/09 (ApexRadiance_LOG + LightDiag): the
//    repeated "7 edited" of lot 7D6F0019FAF78910 were its 7 DISABLED type-3 lamps (flags 0x35 / 0xB5), never baked;
//  - automatic changes (on / off, dimming, recolouring) of a lamp that already changed 3 times within 60 s are ignored:
//    the lamp is "animated" (motion or timer lights, colour-cycling lights) and its current state goes into the next
//    rebuild made for any other reason;
//  - its lot is settled: seen in every enumeration for at least 10 s, and no uncounted change on it for 5 s (a lot that
//    is still loading keeps adding lamps and so never becomes settled while it trickles in);
//  - at most 8 changes in the enumeration (more = lamps switching at dusk / dawn, or streaming in bulk);
//  - removals: the lot is still there in the NEXT enumeration and lost no more lamps (a lot unloading lamp by lamp, or
//    vanishing, is streaming out). The removal of a lot's last lamp is therefore never counted (the lot vanishes).
// Lots streaming in and out must never look like edits (NOTAS 1c: a rebuild every ~30 s from streaming lamps).
// Additions, removals and moves are "user-driven" (Build mode: in the 28-29/09 logs no lamp was ever added or removed
// by itself); the rest is "automatic". The dev build logs what changed on each lamp.
using Clock = std::chrono::steady_clock;
struct LotLampState {
    uint64_t lot = 0;
    int type = 0;
    BYTE flags = 0;
    float col[3] = {}, inten = 0.0f, range = 0.0f, pos[3] = {};
    bool plain = false; // type 3..6
    bool baked = false; // InBake
    // automatic changes of this lamp in the current 60 s window (carried from enumeration to enumeration)
    int autoChanges = 0;
    Clock::time_point autoWindow{};
    bool animated = false;
};
struct LotSeen {
    Clock::time_point firstSeen{}, lastUncounted{};
    bool removalPending = false;
};
std::map<uintptr_t, LotLampState> g_lotLampSig;
std::map<uint64_t, LotSeen> g_lotSeen;
std::atomic<int> g_lotLampEdits{0}, g_lotLampUserEdits{0};
int g_lotChangesCounted = 0, g_lotChangesIgnored = 0;
int g_lampChangesOutside = 0, g_lampChangesNoise = 0, g_lampChangesAnimated = 0, g_lampsAnimated = 0;
std::string g_lastLotChange = "none";
std::map<uint64_t, Clock::time_point> g_quietLogAt; // dev log throttle of the changes that do not count, per lot
LotLightBridge::BakeSnapshot g_bakeSnap;
int g_lampEnumerations = 0;
bool g_lampRefreshNow = false;
std::vector<uint64_t> g_lastUserLots; // lots of the last counted user-driven changes

constexpr float kMoveTol = 0.05f;   // metres
constexpr float kLightRel = 0.05f;  // relative change of colour x intensity x range, per channel
constexpr float kLightAbs = 0.05f;  // absolute floor (colour x intensity x range: typical lamps give 5..200)
constexpr int kAnimatedChanges = 3; // automatic changes within kAnimatedWindow: the lamp is animated
constexpr auto kAnimatedWindow = std::chrono::seconds(60);
constexpr auto kQuietLogEvery = std::chrono::seconds(60);

bool IsPlainType(int type) { return type >= 3 && type <= 6; }

// Lot light of a type the bake can take (3..6 or the street-lamp class 0xB), alive, outdoors (room known, room 0)
bool ReadLotLamp(uintptr_t L, LotLampState& out) {
    __try {
        const uint32_t lo = *reinterpret_cast<const uint32_t*>(L + 0xC0), hi = *reinterpret_cast<const uint32_t*>(L + 0xC4);
        if ((lo | hi) == 0) return false; // not a lot lamp
        const BYTE f = *reinterpret_cast<const BYTE*>(L + 0x100);
        if (!(f & 0x01) || !(f & 0x04) || *reinterpret_cast<const int*>(L + 0x08) != 0) return false; // alive, outdoors
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        if (!IsPlainType(type) && type != 0xB) return false; // window lights and the other classes never reach the bake
        std::memcpy(out.col, reinterpret_cast<const void*>(L + 0xF0), 12);  // base colour
        out.inten = *reinterpret_cast<const float*>(L + 0x10);               // intensity (x)
        out.range = *reinterpret_cast<const float*>(L + 0x130);              // range
        std::memcpy(out.pos, reinterpret_cast<const void*>(L + 0x120), 12); // position
        out.lot = (static_cast<uint64_t>(hi) << 32) | lo;
        out.type = type;
        out.flags = f;
        out.plain = IsPlainType(type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float LampLight(const LotLampState& s, int c) { return s.col[c] * s.inten * s.range; }

// The terrain bake takes this lamp now (see the block comment). Lot lamps 3..6: TerrainLightTest (enabled 0x40, lit
// 0x20). Street-lamp class: the game's vfunc+0x20 test is assumed to need the lit flag too (unverified,
// terrain-and-light-bake.md section 10; a save loaded by day keeps a lamps-off terrain light, which fits). A lamp whose
// light is zero (intensity or range 0, black colour) draws nothing.
bool InBake(const LotLampState& s) {
    if (!(s.flags & 0x20)) return false;
    if (s.plain && !(s.flags & 0x40)) return false;
    const float w = s.inten * s.range;
    if (!std::isfinite(w) || !(w > 1e-3f)) return false;
    return std::max({s.col[0], s.col[1], s.col[2]}) > 1e-3f;
}

bool MovedApart(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return !(dx * dx + dy * dy + dz * dz <= kMoveTol * kMoveTol); // NaN counts as moved
}

bool LightDiffers(const float* a, const float* b) {
    for (int c = 0; c < 3; c++) {
        const float tol = std::max(kLightAbs, kLightRel * std::max(std::fabs(a[c]), std::fabs(b[c])));
        if (!(std::fabs(a[c] - b[c]) <= tol)) return true;
    }
    return false;
}

bool RawChanged(const LotLampState& a, const LotLampState& b) {
    return a.flags != b.flags || a.type != b.type || std::memcmp(a.col, b.col, sizeof a.col) != 0 || std::memcmp(&a.inten, &b.inten, 4) != 0 ||
           std::memcmp(&a.range, &b.range, 4) != 0 || std::memcmp(a.pos, b.pos, sizeof a.pos) != 0;
}

// Developer log: what changed on one lamp ("L1234ABCD type 3: lit 1->0, intensity 1.00->0.00 [leaves the bake]")
std::string LampChangeText(uintptr_t L, const LotLampState& a, const LotLampState& b) {
    std::string t = std::format("L{:08X} type {}:", L, b.type);
    const BYTE df = a.flags ^ b.flags;
    if (df & 0x20) t += std::format(" lit {}->{},", (a.flags & 0x20) ? 1 : 0, (b.flags & 0x20) ? 1 : 0);
    if (df & 0x40) t += std::format(" enabled {}->{},", (a.flags & 0x40) ? 1 : 0, (b.flags & 0x40) ? 1 : 0);
    if (df & ~0x60) t += std::format(" flags {:02X}->{:02X},", a.flags, b.flags);
    if (std::memcmp(&a.inten, &b.inten, 4) != 0) t += std::format(" intensity {:.3f}->{:.3f},", a.inten, b.inten);
    if (std::memcmp(a.col, b.col, sizeof a.col) != 0)
        t += std::format(" colour ({:.2f} {:.2f} {:.2f})->({:.2f} {:.2f} {:.2f}),", a.col[0], a.col[1], a.col[2], b.col[0], b.col[1], b.col[2]);
    if (std::memcmp(&a.range, &b.range, 4) != 0) t += std::format(" range {:.2f}->{:.2f},", a.range, b.range);
    if (std::memcmp(a.pos, b.pos, sizeof a.pos) != 0) {
        const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
        t += std::format(" moved {:.3f} m,", std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (t.back() == ',') t.pop_back();
    if (a.baked != b.baked) t += b.baked ? " [enters the bake]" : " [leaves the bake]";
    return t;
}

void AddDetail(std::string& d, int& n, const std::string& t) {
    if (n < 6) d += (d.empty() ? "" : "; ") + t;
    else if (n == 6) d += "; ...";
    n++;
}

void TrackLotLampEdits() {
    const auto now = Clock::now();
    std::map<uintptr_t, LotLampState> cur;
    for (uintptr_t L : g_enumLights) {
        LotLampState s;
        if (ReadLotLamp(L, s)) {
            s.baked = InBake(s);
            cur.emplace(L, s);
        }
    }
    std::map<uint64_t, int> lotsNow;
    for (const auto& [L, s] : cur) lotsNow[s.lot]++;
    // per lot: counted additions / edits / removals in this enumeration, and (dev) what changed
    struct Change {
        int added = 0, edited = 0, moved = 0, removed = 0;
        std::string detail;
        int details = 0;
    };
    std::map<uint64_t, Change> changes;
    struct Quiet { // dev log: changes that do not count
        std::string detail;
        int details = 0;
    };
    std::map<uint64_t, Quiet> quiet;
    int total = 0;
    for (auto& [L, s] : cur) {
        auto it = g_lotLampSig.find(L);
        if (it == g_lotLampSig.end()) {
            if (s.baked) {
                Change& c = changes[s.lot];
                c.added++;
                total++;
                if constexpr (!kPublicBuild) AddDetail(c.detail, c.details, std::format("L{:08X} type {}: added", L, s.type));
            }
            continue;
        }
        const LotLampState& p = it->second;
        s.autoChanges = p.autoChanges;
        s.autoWindow = p.autoWindow;
        s.animated = p.animated;
        if (!RawChanged(p, s)) continue;
        const char* why = nullptr; // why a raw change does not count
        bool counts = false, moved = false;
        float pl[3], sl[3];
        for (int c = 0; c < 3; c++) {
            pl[c] = LampLight(p, c);
            sl[c] = LampLight(s, c);
        }
        if (p.baked && s.baked && MovedApart(p.pos, s.pos)) {
            counts = moved = true; // user-driven (Build mode)
        } else if (p.baked != s.baked || (p.baked && s.baked && LightDiffers(pl, sl))) {
            // automatic: switched on / off, dimmed, recoloured
            if (now - s.autoWindow > kAnimatedWindow) {
                s.autoWindow = now;
                s.autoChanges = 0;
            }
            if (++s.autoChanges >= kAnimatedChanges && !s.animated) {
                s.animated = true;
                g_lampsAnimated++;
                if constexpr (!kPublicBuild)
                    LOG_INFO(std::format("[LotLightBridge] Lamp L{:08X} (type {}) on lot {:016X} switches or dims by itself ({} changes within {} s): its changes no longer "
                                         "rebuild the terrain (the next rebuild takes its state)",
                                         L, s.type, s.lot, s.autoChanges, kAnimatedWindow.count()));
            }
            if (s.animated) {
                why = "animated";
                g_lampChangesAnimated++;
            } else
                counts = true;
        } else if (p.baked || s.baked) {
            why = "below the threshold";
            g_lampChangesNoise++;
        } else {
            why = "not in the bake";
            g_lampChangesOutside++;
        }
        if (counts) {
            Change& c = changes[s.lot];
            c.edited++;
            if (moved) c.moved++;
            total++;
            if constexpr (!kPublicBuild) AddDetail(c.detail, c.details, LampChangeText(L, p, s));
        } else if constexpr (!kPublicBuild) {
            Quiet& q = quiet[s.lot];
            AddDetail(q.detail, q.details, LampChangeText(L, p, s) + " (" + why + ")");
        }
    }
    for (const auto& [L, p] : g_lotLampSig)
        if (!cur.count(L) && p.baked) {
            Change& c = changes[p.lot];
            c.removed++;
            total++;
            if constexpr (!kPublicBuild) AddDetail(c.detail, c.details, std::format("L{:08X} type {}: removed", L, p.type));
        }
    g_lotLampSig.swap(cur);

    // lots seen: new lots start their settle time, vanished lots are forgotten (with any pending removal)
    for (const auto& [lot, n] : lotsNow)
        if (!g_lotSeen.count(lot)) g_lotSeen[lot] = LotSeen{now, now, false};
    for (auto it = g_lotSeen.begin(); it != g_lotSeen.end();) {
        if (!lotsNow.count(it->first)) it = g_lotSeen.erase(it);
        else ++it;
    }
    for (auto it = g_quietLogAt.begin(); it != g_quietLogAt.end();) {
        if (!lotsNow.count(it->first)) it = g_quietLogAt.erase(it);
        else ++it;
    }

    int counted = 0, ignored = 0;
    bool userDriven = false;
    std::vector<uint64_t> userLots;
    std::string what;
    // removals seen last time: confirmed when the lot is still here and lost no more lamps
    for (auto& [lot, seen] : g_lotSeen) {
        if (!seen.removalPending) continue;
        auto c = changes.find(lot);
        if (c != changes.end() && c->second.removed > 0) continue; // still losing lamps: wait
        seen.removalPending = false;
        counted++;
        userDriven = true;
        userLots.push_back(lot);
        what = std::format("lamp removed on lot {:016X} (user-driven)", lot);
    }
    const bool bulk = total > 8;
    for (const auto& [lot, c] : changes) {
        auto s = g_lotSeen.find(lot);
        if (s == g_lotSeen.end()) { // the lot vanished: streaming out
            ignored++;
            continue;
        }
        LotSeen& seen = s->second;
        const bool settled = now - seen.firstSeen >= std::chrono::seconds(10) && now - seen.lastUncounted >= std::chrono::seconds(5);
        if (!settled || bulk) {
            seen.lastUncounted = now;
            seen.removalPending = false;
            ignored++;
            continue;
        }
        if (c.removed > 0) seen.removalPending = true; // confirmed at the next enumeration
        if (c.added > 0 || c.edited > 0) {
            counted++;
            const bool user = c.added > 0 || c.moved > 0;
            userDriven |= user;
            if (user) userLots.push_back(lot);
            what = std::format("lot {:016X}: {} added, {} edited ({} moved), {} removed ({})", lot, c.added, c.edited, c.moved, c.removed, user ? "user-driven" : "automatic");
            if (!c.detail.empty()) what += ": " + c.detail;
        }
    }
    g_lotChangesIgnored += ignored;
    if (counted > 0) {
        g_lotChangesCounted += counted;
        g_lastLotChange = what;
        if (userDriven) {
            g_lastUserLots = std::move(userLots);
            g_lotLampUserEdits.fetch_add(1, std::memory_order_relaxed);
        }
        g_lotLampEdits.fetch_add(1, std::memory_order_relaxed);
        if constexpr (!kPublicBuild) LOG_INFO("[LotLightBridge] Lot lamp change: " + what);
    } else if (ignored > 0) {
        if constexpr (!kPublicBuild)
            LOG_DEBUG(std::format("[LotLightBridge] Lot lamp changes ignored ({} lots; {}): streaming, lots still loading or lamps switching together", ignored,
                                  bulk ? "bulk" : "not settled"));
    }
    if constexpr (!kPublicBuild) {
        // what changed but does not rebuild (at most once a minute per lot): the diagnosis of lamps that keep changing
        for (const auto& [lot, q] : quiet) {
            auto& at = g_quietLogAt[lot];
            if (at != Clock::time_point{} && now - at < kQuietLogEvery) continue;
            at = now;
            LOG_INFO(std::format("[LotLightBridge] Lamp changes that do not rebuild the terrain, lot {:016X}: {}", lot, q.detail));
        }
    }

    // snapshot for the terrain relight: every tracked lamp (baked or not, so a lamp switched off still matches its
    // baked self by position), sorted by lot; the lots and the settled lots
    g_bakeSnap.lamps.clear();
    g_bakeSnap.lots.clear();
    g_bakeSnap.settledLots.clear();
    for (const auto& [L, s] : g_lotLampSig) {
        LotLightBridge::BakeLamp b;
        b.lot = s.lot;
        b.type = s.type;
        std::memcpy(b.pos, s.pos, sizeof b.pos);
        for (int c = 0; c < 3; c++) b.light[c] = LampLight(s, c);
        b.baked = s.baked;
        b.animated = s.animated;
        g_bakeSnap.lamps.push_back(b);
    }
    std::stable_sort(g_bakeSnap.lamps.begin(), g_bakeSnap.lamps.end(), [](const LotLightBridge::BakeLamp& a, const LotLightBridge::BakeLamp& b) { return a.lot < b.lot; });
    for (const auto& [lot, n] : lotsNow) g_bakeSnap.lots.push_back(lot); // std::map: already sorted
    for (const auto& [lot, seen] : g_lotSeen)
        if (now - seen.firstSeen >= std::chrono::seconds(10) && now - seen.lastUncounted >= std::chrono::seconds(5)) g_bakeSnap.settledLots.push_back(lot);
    g_lampEnumerations++;
}

// Picks up to 16 lamps whose light can reach the roof piece at world position (x, z): chosen per draw from the roof
// position, so the camera (zoom, rotation) never changes which lamps light a roof.
int g_lastLampCandidates = 0; // lamps within reach at the last SelectLamps (light probe detail)
int SelectLamps(float x, float z, float maxScore) {
    struct Cand { float score; const std::array<float, 8>* v; };
    Cand c[64];
    int n = 0;
    for (const auto& v : g_allLamps) {
        const float dx = v[0] - x, dz = v[2] - z;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float score = d - v[3];
        if (score > maxScore) continue; // too far from the lamp's reach
        if (n < 64) c[n++] = {score, &v};
        else {
            int worst = 0;
            for (int k = 1; k < 64; k++)
                if (c[k].score > c[worst].score) worst = k;
            if (score < c[worst].score) c[worst] = {score, &v};
        }
    }
    g_lastLampCandidates = n;
    const int m = std::min(16, n);
    std::partial_sort(c, c + m, c + n, [](const Cand& a, const Cand& b) { return a.score < b.score; });
    std::memset(g_lampData, 0, sizeof(float) * 4 * 32);
    for (int k = 0; k < m; k++) {
        std::memcpy(g_lampData[k], c[k].v->data(), 16);
        std::memcpy(g_lampData[16 + k], c[k].v->data() + 4, 16);
    }
    return m;
}

template <typename DrawFn> bool DrawRoof(IDirect3DDevice9* dev, DrawFn draw);

void EnsureReplacement(IDirect3DDevice9* dev) {
    if (g_replacementPs || g_compileTried) return;
    g_compileTried = true;
    const std::string err = CompilePs(dev, kReplacementPsId, &g_replacementPs);
    g_status = err.empty() ? "Active" : "Failed: " + err;
    LOG_INFO("[LotLightBridge] " + g_status);
}

void EnsureObjectReplacement(IDirect3DDevice9* dev) {
    if (g_objectPs || g_objectCompileTried) return;
    g_objectCompileTried = true;
    const std::string err = CompilePs(dev, kObjectRigPsId, &g_objectPs);
    LOG_INFO("[LotLightBridge] Object shadow fix: " + (err.empty() ? std::string("active") : err));
}

template <typename DrawFn> bool DrawRoof(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_roofFix.load(std::memory_order_relaxed) || !g_curVsIsRoof) return false;
    float world[12];
    if (FAILED(dev->GetVertexShaderConstantF(8, world, 3))) return false; // c8..c10 = world matrix rows, .w = translation
    if (!g_roofPs && !g_roofCompileTried) {
        g_roofCompileTried = true;
        const std::string err = CompilePs(dev, kRoofPsId, &g_roofPs);
        LOG_INFO("[LotLightBridge] Roofs: " + (err.empty() ? std::string("active") : err));
    }
    if (!g_roofPs) return false;
    const int picked = SelectLamps(world[3], world[11], 80.0f);
    g_lampData[32][0] = g_roofStrength.load(std::memory_order_relaxed);
    g_lampData[32][1] = static_cast<float>(picked);
    float saved[33][4];
    dev->GetPixelShaderConstantF(20, &saved[0][0], 33);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    dev->SetPixelShader(g_roofPs);
    dev->SetPixelShaderConstantF(20, &g_lampData[0][0], 33);
    draw();
    dev->SetPixelShaderConstantF(20, &saved[0][0], 33);
    dev->SetPixelShader(original);
    g_inOwnCall = false;
    g_roofDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Lakes: the game's lake water only reflects a fixed sky cube and gets no lamp light. After the game draws it, an
// additive pass on the same geometry adds lamp reflections and glow (the game's shader and states stay untouched). ----
std::atomic<bool> g_waterFix{false};
std::atomic<float> g_waterStrength{1.0f};
std::atomic<float> g_waterRefl{1.0f};
IDirect3DPixelShader9* g_waterPs = nullptr;
bool g_waterCompileTried = false;
std::atomic<int> g_waterDrawn{0};

template <typename DrawFn> bool DrawLake(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_waterFix.load(std::memory_order_relaxed) || !g_curVsIsLake) return false;
    if (!g_waterPs && !g_waterCompileTried) {
        g_waterCompileTried = true;
        const std::string err = CompilePs(dev, kWaterPsId, &g_waterPs);
        LOG_INFO("[LotLightBridge] Water: " + (err.empty() ? std::string("active") : err));
    }
    if (!g_waterPs) return false;
    float world[12];
    if (FAILED(dev->GetVertexShaderConstantF(8, world, 3))) return false;
    g_inOwnCall = true;
    draw(); // the game's water, unchanged
    const int picked = SelectLamps(world[3], world[11], 150.0f);
    {
        g_lampData[32][0] = g_waterStrength.load(std::memory_order_relaxed);
        g_lampData[32][1] = static_cast<float>(picked);
        float consts[40][4] = {};
        std::memcpy(consts, g_lampData, sizeof(float) * 4 * 33);
        dev->GetVertexShaderConstantF(4, &consts[33][0], 4); // world-view-projection of the water mesh
        // The shader projects world positions, so turn local->clip into world->clip: M * inverse(World). The water mesh
        // of a rotated lot has a rotated world matrix (rows c8..c10), so subtracting the translation is not enough.
        {
            const float a = world[0], b = world[1], c = world[2], d = world[4], e = world[5], f = world[6], g = world[8], h = world[9], k = world[10];
            const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
            if (std::fabs(det) > 1e-8f) {
                const float id = 1.0f / det;
                float inv[4][4] = {{(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id, 0},
                                   {(f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id, 0},
                                   {(d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id, 0},
                                   {0, 0, 0, 1}};
                const float t[3] = {world[3], world[7], world[11]};
                for (int r = 0; r < 3; r++) inv[r][3] = -(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]);
                float vp[4][4];
                for (int r = 0; r < 4; r++)
                    for (int col = 0; col < 4; col++)
                        vp[r][col] = consts[33 + r][0] * inv[0][col] + consts[33 + r][1] * inv[1][col] + consts[33 + r][2] * inv[2][col] + consts[33 + r][3] * inv[3][col];
                std::memcpy(consts[33], vp, sizeof(vp)); // translation c57 stays 0: positions are world positions
            } else {
                consts[37][0] = world[3]; consts[37][1] = world[7]; consts[37][2] = world[11];
            }
        }
        consts[38][0] = g_waterRefl.load(std::memory_order_relaxed);
        // Scene depth for the reflection ray march: Depth Blur's INTZ, readable once it is unbound as depth-stencil.
        IDirect3DTexture9* depthTex = DepthShare::Texture();
        IDirect3DSurface9* curDs = nullptr;
        ExtraHooks::RawGetDepthStencilSurface(dev, &curDs);
        const bool useDepth = depthTex && curDs && curDs == DepthShare::Surface();
        if (useDepth) {
            // device z = A + B / w, from the projection rows: row2 = A * row3 + (0, 0, 0, B)
            const float* r2 = consts[35];
            const float* r3 = consts[36];
            const float d33 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
            const float A = d33 > 1e-12f ? (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / d33 : 1.0f;
            consts[39][0] = A;
            consts[39][1] = r2[3] - A * r3[3];
            consts[39][2] = 1.0f;
        }
        float saved[40][4];
        dev->GetPixelShaderConstantF(20, &saved[0][0], 40);
        IDirect3DBaseTexture9* old7 = nullptr;
        DWORD s7u = 0, s7v = 0, s7min = 0, s7mag = 0, s7mip = 0, s7srgb = 0, zen = 0;
        if (useDepth) {
            dev->GetTexture(7, &old7);
            dev->GetSamplerState(7, D3DSAMP_ADDRESSU, &s7u);
            dev->GetSamplerState(7, D3DSAMP_ADDRESSV, &s7v);
            dev->GetSamplerState(7, D3DSAMP_MINFILTER, &s7min);
            dev->GetSamplerState(7, D3DSAMP_MAGFILTER, &s7mag);
            dev->GetSamplerState(7, D3DSAMP_MIPFILTER, &s7mip);
            dev->GetSamplerState(7, D3DSAMP_SRGBTEXTURE, &s7srgb);
            dev->GetRenderState(D3DRS_ZENABLE, &zen);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            ExtraHooks::RawSetDepthStencilSurface(dev, nullptr);
            dev->SetTexture(7, depthTex);
            dev->SetSamplerState(7, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(7, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(7, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(7, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(7, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(7, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        DWORD ab, sb, db, bo, sep, zw, cw, at;
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        dev->GetRenderState(D3DRS_SRCBLEND, &sb);
        dev->GetRenderState(D3DRS_DESTBLEND, &db);
        dev->GetRenderState(D3DRS_BLENDOP, &bo);
        dev->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &sep);
        dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
        dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
        IDirect3DPixelShader9* original = g_curPs;
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); // premultiplied: reflection * a + lamps
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetPixelShader(g_waterPs);
        dev->SetPixelShaderConstantF(20, &consts[0][0], 40);
        DepthShare::SetInternalPass(true); // ZENABLE is off here: not the first UI draw for Depth Blur
        draw();
        DepthShare::SetInternalPass(false);
        dev->SetPixelShaderConstantF(20, &saved[0][0], 40);
        if (useDepth) {
            dev->SetTexture(7, old7);
            if (old7) old7->Release();
            dev->SetSamplerState(7, D3DSAMP_ADDRESSU, s7u);
            dev->SetSamplerState(7, D3DSAMP_ADDRESSV, s7v);
            dev->SetSamplerState(7, D3DSAMP_MINFILTER, s7min);
            dev->SetSamplerState(7, D3DSAMP_MAGFILTER, s7mag);
            dev->SetSamplerState(7, D3DSAMP_MIPFILTER, s7mip);
            dev->SetSamplerState(7, D3DSAMP_SRGBTEXTURE, s7srgb);
            ExtraHooks::RawSetDepthStencilSurface(dev, curDs);
            dev->SetRenderState(D3DRS_ZENABLE, zen);
        }
        if (curDs) curDs->Release();
        dev->SetPixelShader(original);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, at);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, cw);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zw);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, sep);
        dev->SetRenderState(D3DRS_BLENDOP, bo);
        dev->SetRenderState(D3DRS_DESTBLEND, db);
        dev->SetRenderState(D3DRS_SRCBLEND, sb);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, ab);
        g_waterDrawn.fetch_add(1, std::memory_order_relaxed);
    }
    g_inOwnCall = false;
    return true;
}

// ---- Snow: the lot light pass has a much bigger snow variant (snow normals, snow mask). Instead of rewriting it, its
// bytecode is patched: after "texld r0, v3, s2" (lot light map) and the following "mul r0.xyz, r1.w, r0" (light basis
// factor) insert "texld r7, v7(TEXCOORD1), s12", r7 x4 and "max r0.xyz, r0, r7", so it takes the brighter of the lot
// light map and the terrain light map. The x4 undoes the "mul r0.xyz, r4, c9.x" (0.25) this variant applies to lamp
// light further down; the world snow terrain uses the light map x c7.x (1). Its VS outputs the terrain uv in TEXCOORD1
// (c15) and gets the chunk centre in c16. ----
std::vector<DWORD> ShaderCode(IDirect3DPixelShader9* ps);
IDirect3DPixelShader9* g_snowPs = nullptr;
bool g_snowTried = false;
std::atomic<int> g_snowDrawn{0};

bool PatchSnowBytecode(std::vector<DWORD>& t) {
    auto regNum = [](DWORD r) { return r & 0x7FF; };
    auto regType = [](DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); };
    size_t dclEnd = 0, texldEnd = 0;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        const size_t len = (tok >> 24) & 0xF;
        const DWORD op = tok & 0xFFFF;
        if (i + len >= t.size()) return false;
        if (op == 0x1F && regType(t[i + 2]) == 10 && regNum(t[i + 2]) == 11) dclEnd = i + 1 + len;
        if (op == 0x42 && !texldEnd && regType(t[i + 1]) == 0 && regNum(t[i + 1]) == 0 && regType(t[i + 2]) == 1 && regNum(t[i + 2]) == 3 && regType(t[i + 3]) == 10 && regNum(t[i + 3]) == 2)
            texldEnd = i + 1 + len;
        i += 1 + len;
    }
    if (!dclEnd || !texldEnd || texldEnd < dclEnd || texldEnd + 1 >= t.size()) return false;
    // next instruction must be "mul r0.xyz, r1.w, r0" (light basis factor)
    if ((t[texldEnd] & 0xFFFF) != 0x05 || regType(t[texldEnd + 1]) != 0 || regNum(t[texldEnd + 1]) != 0) return false;
    const size_t insertAt = texldEnd + 1 + ((t[texldEnd] >> 24) & 0xF);
    const DWORD fetch[] = {0x03000042, 0x802F0007, 0x90E40007, 0xA0E4080C,  // texld_pp r7, v7, s12
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,  // add_pp r7.xyz, r7, r7
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,  // add_pp r7.xyz, r7, r7
                           0x0300000B, 0x80270000, 0x80E40000, 0x80E40007}; // max_pp r0.xyz, r0, r7
    t.insert(t.begin() + insertAt, std::begin(fetch), std::end(fetch));
    const DWORD decl[] = {0x0200001F, 0x80010005, 0x90230007, 0x0200001F, 0x90000000, 0xA00F080C};
    t.insert(t.begin() + dclEnd, std::begin(decl), std::end(decl));
    return true;
}

template <typename DrawFn> bool DrawLotSnow(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_curVsIsSnowLot) return false;
    if (!g_snowPs && !g_snowTried) {
        g_snowTried = true;
        // patched from the shader the game has bound (this runs only for PsClass::LotLightSnow, an exact match)
        std::vector<DWORD> t = ShaderCode(g_curPs);
        const bool ok = !t.empty() && PatchSnowBytecode(t) && SUCCEEDED(dev->CreatePixelShader(t.data(), &g_snowPs));
        if (!ok) g_snowPs = nullptr;
        LOG_INFO(std::string("[LotLightBridge] Snow: ") + (ok ? "active" : "failed"));
    }
    if (!g_snowPs) return false;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(15, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return false;
    // The terrain light the lot is compared with: the world atlas when it is ready (a lot can reach past its "home"
    // chunk, c16: LightProbe-m76, lot at z 1290 on the chunk ending at z 1280, drew the chunk's clamped edge row and
    // came out dark next to a lit sidewalk), else the home chunk's own map as before. The VS computes the uv as
    // (world.xz - c16.xz) * c15.xy + c15.zw, so for the atlas (uv = world.xz * a.xy + a.zw) c15 becomes
    // (a.xy, a.zw + c16.xz * a.xy) for this draw.
    float atlasC[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(atlasC);
    IDirect3DBaseTexture9* terrain = atlas;
    if (!atlas) {
        auto it = g_chunks.find(Key(v[4], v[6])); // c16.xz = chunk centre
        if (it == g_chunks.end() || !it->second.tex) {
            g_lotMissing.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        terrain = ChunkTexture(it->first, it->second.tex);
    }
    const float atlasMap[4] = {atlasC[0], atlasC[1], atlasC[2] + v[4] * atlasC[0], atlasC[3] + v[6] * atlasC[1]};
    IDirect3DPixelShader9* original = g_curPs;
    IDirect3DBaseTexture9* old12 = nullptr;
    dev->GetTexture(12, &old12);
    DWORD au, av, mn, mg, mp, srgb;
    dev->GetSamplerState(12, D3DSAMP_ADDRESSU, &au);
    dev->GetSamplerState(12, D3DSAMP_ADDRESSV, &av);
    dev->GetSamplerState(12, D3DSAMP_MINFILTER, &mn);
    dev->GetSamplerState(12, D3DSAMP_MAGFILTER, &mg);
    dev->GetSamplerState(12, D3DSAMP_MIPFILTER, &mp);
    dev->GetSamplerState(12, D3DSAMP_SRGBTEXTURE, &srgb);
    g_inOwnCall = true;
    dev->SetPixelShader(g_snowPs);
    dev->SetTexture(12, terrain);
    if (atlas) dev->SetVertexShaderConstantF(15, atlasMap, 1);
    dev->SetSamplerState(12, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(12, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(12, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(12, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(12, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(12, D3DSAMP_SRGBTEXTURE, FALSE);
    draw();
    dev->SetSamplerState(12, D3DSAMP_SRGBTEXTURE, srgb);
    dev->SetSamplerState(12, D3DSAMP_MIPFILTER, mp);
    dev->SetSamplerState(12, D3DSAMP_MAGFILTER, mg);
    dev->SetSamplerState(12, D3DSAMP_MINFILTER, mn);
    dev->SetSamplerState(12, D3DSAMP_ADDRESSV, av);
    dev->SetSamplerState(12, D3DSAMP_ADDRESSU, au);
    dev->SetTexture(12, old12);
    if (old12) old12->Release();
    if (atlas) dev->SetVertexShaderConstantF(15, v, 1);
    dev->SetPixelShader(original);
    g_inOwnCall = false;
    g_snowDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Roads. Every road piece (straight road + sidewalk, lane markings, corners, the edge blended into the terrain: 4
// shader variants seen so far) is drawn with the same vertex shader (shader_ids.h kRoadVs). It samples its own
// copy of the chunk light map with the terrain uv (local xz / 256 + 0.5, VS c16) and the chunk centre in the world
// matrix (VS c8.w, c10.w). That copy does not get the lamps the world terrain map has, so roads stayed dark next to lit
// ground. Any pixel shader drawn with that vertex shader is patched by pattern (ShaderPatches::PatchRoad): max with the
// terrain map on a free sampler; with the smoothed map ready it also replaces the road's own copy. Where the shader has
// the sidewalk snow blend, "Calcada com neve pisada" mixes the plain road texture back on the bright parts.
struct PatchedPs {
    IDirect3DPixelShader9* ps = nullptr;
    bool tried = false;
    ShaderPatches::RoadPatch road;
    ShaderPatches::FloorPatch floor;
    ShaderPatches::InstancedPatch inst;
    ShaderPatches::SnowCoverPatch snow;
    ShaderPatches::ObjectLampPatch obj;
    DWORD nightConst = 0;
};
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_roadPs, g_floorPs, g_snowFloorPs, g_snowFloorPs0, g_leafPs, g_fencePs, g_snowCoverPs, g_snowReliefPs, g_objLampPs;
std::atomic<int> g_roadDrawn{0}, g_floorDrawn{0}, g_snowFloorDrawn{0}, g_leafDrawn{0}, g_roofSnowDrawn{0}, g_foliageDrawn{0}, g_fenceDrawn{0}, g_snowCoverDrawn{0}, g_snowReliefDrawn{0}, g_objLampDrawn{0};
std::atomic<bool> g_objPixel{true};
std::atomic<float> g_objPixelStrength{1.0f};
std::atomic<bool> g_objPixelLamps{true};          // outdoor rig objects: world lamps per pixel instead of the rig lamps
std::atomic<float> g_objPixelLampStrength{1.0f};
std::atomic<bool> g_fenceFix{true};
std::atomic<float> g_fenceStrength{1.0f};
std::atomic<float> g_sidewalkClear{0.5f};

std::vector<DWORD> ShaderCode(IDirect3DPixelShader9* ps) {
    UINT size = 0;
    if (!ps || FAILED(ps->GetFunction(nullptr, &size)) || size < 8 || size > 65536) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(ps->GetFunction(t.data(), &size))) return {};
    return t;
}

// Development build: keeps the bytecode of every shader pair a fix refused, in Apex Radiance\ShadersRecusados, so it can be
// studied offline (the log only has the shader's address, which changes between sessions).
void SaveRefused(const char* what, const std::vector<DWORD>& ps) {
    if constexpr (kPublicBuild) return;
    try {
        std::string tag;
        for (const char* c = what; *c; ++c) tag += std::isalnum(static_cast<unsigned char>(*c)) ? *c : '_';
        // Named by content (the address changes every session), written once, at most 300 files per session.
        static int written = 0;
        if (written >= 300) return;
        auto hash = [](const std::vector<DWORD>& t) {
            uint32_t h = 2166136261u;
            for (DWORD d : t) h = (h ^ d) * 16777619u;
            return h;
        };
        const std::filesystem::path dir = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ShadersRecusados";
        const auto save = [&](const std::string& name, const std::vector<DWORD>& data) {
            if (std::filesystem::exists(dir / name)) return;
            std::filesystem::create_directories(dir);
            std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * 4));
            written++;
        };
        const uint32_t psHash = hash(ps);
        save(std::format("{}_PS_{:08X}.bin", tag, psHash), ps);
        UINT size = 0;
        if (g_curVs && SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
            std::vector<DWORD> vs(size / 4);
            if (SUCCEEDED(g_curVs->GetFunction(vs.data(), &size))) save(std::format("{}_PS_{:08X}_VS_{:08X}.bin", tag, psHash, hash(vs)), vs);
        }
    } catch (...) {
    }
}

// Patched copy of the current pixel shader, made once per shader with `patch`.
template <typename PatchFn> PatchedPs& PatchedFor(IDirect3DDevice9* dev, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>& cache, const char* what, PatchFn patch) {
    PatchedPs& p = cache[g_curPs];
    if (!p.tried) {
        p.tried = true;
        std::vector<DWORD> t = ShaderCode(g_curPs);
        const std::vector<DWORD> original = t;
        const bool matched = !t.empty() && patch(t, p);
        const HRESULT hr = matched ? dev->CreatePixelShader(t.data(), &p.ps) : E_FAIL;
        if (matched && SUCCEEDED(hr)) LOG_INFO(std::format("[LotLightBridge] {}: shader {:08X} patched", what, reinterpret_cast<uintptr_t>(g_curPs)));
        else {
            p.ps = nullptr;
            if (matched) LOG_WARNING(std::format("[LotLightBridge] {}: shader {:08X} refused by D3D ({:08X})", what, reinterpret_cast<uintptr_t>(g_curPs), static_cast<uint32_t>(hr)));
            else LOG_INFO(std::format("[LotLightBridge] {}: shader {:08X} does not have the expected pattern, left as the game draws it", what, reinterpret_cast<uintptr_t>(g_curPs)));
            if (!original.empty()) SaveRefused(what, original);
        }
    }
    return p;
}

// Binds `tex` to sampler s with linear filtering and clamp for one draw; restores everything afterwards.
struct SamplerBind {
    IDirect3DDevice9* dev;
    DWORD s;
    IDirect3DBaseTexture9* old = nullptr;
    DWORD st[6] = {};
    static constexpr D3DSAMPLERSTATETYPE kStates[6] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE};
    SamplerBind(IDirect3DDevice9* d, DWORD sampler, IDirect3DBaseTexture9* tex, DWORD mip = D3DTEXF_LINEAR) : dev(d), s(sampler) {
        dev->GetTexture(s, &old);
        for (int i = 0; i < 6; i++) dev->GetSamplerState(s, kStates[i], &st[i]);
        const DWORD v[6] = {D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, mip, FALSE};
        dev->SetTexture(s, tex);
        for (int i = 0; i < 6; i++) dev->SetSamplerState(s, kStates[i], v[i]);
    }
    ~SamplerBind() {
        for (int i = 5; i >= 0; i--) dev->SetSamplerState(s, kStates[i], st[i]);
        dev->SetTexture(s, old);
        if (old) old->Release();
    }
};

template <typename DrawFn> bool DrawRoad(IDirect3DDevice9* dev, DrawFn draw) {
    PatchedPs& p = PatchedFor(dev, g_roadPs, "Road", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchRoad(t, pp.road); });
    if (!p.ps) return false;
    float m[4], w[12];
    if (FAILED(dev->GetVertexShaderConstantF(g_curRoadMap, m, 1)) || !Near(m[0], 1.0f / 256.0f) || !Near(m[1], 1.0f / 256.0f) || !Near(m[2], 0.5f) || !Near(m[3], 0.5f)) return false;
    if (FAILED(dev->GetVertexShaderConstantF(8, w, 3))) return false;
    auto it = g_chunks.find(Key(w[3], w[11])); // world matrix translation = chunk centre
    if (it == g_chunks.end() || !it->second.tex) {
        g_lotMissing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // With the smoothed map ready, the road's own copy (the blocky DXT5 one) is replaced by it too, so roads and
    // sidewalks get exactly the same clean light as the ground next to them.
    IDirect3DTexture9* smooth = LightmapSmooth::Find(it->first);
    IDirect3DBaseTexture9* oldLight = nullptr;
    if (smooth) dev->GetTexture(p.road.lightSampler, &oldLight);
    float oldC[4] = {};
    const bool sidewalk = p.road.sidewalkConst >= 0;
    if (sidewalk) dev->GetPixelShaderConstantF(p.road.sidewalkConst, oldC, 1);
    const float c[4] = {g_sidewalkClear.load(std::memory_order_relaxed), 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind terrain(dev, p.road.extraSampler, ChunkTexture(it->first, it->second.tex));
        if (smooth) dev->SetTexture(p.road.lightSampler, smooth);
        if (sidewalk) dev->SetPixelShaderConstantF(p.road.sidewalkConst, c, 1);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(original);
        if (sidewalk) dev->SetPixelShaderConstantF(p.road.sidewalkConst, oldC, 1);
        if (smooth) dev->SetTexture(p.road.lightSampler, oldLight);
    }
    if (oldLight) oldLight->Release();
    g_inOwnCall = false;
    g_roadDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Snowy floor tiles (shader_ids.h kFloorVs). The pixel shader lights them only with the lot's own light
// map (x the light basis x 0.25), so street lamps never reach them. Its vertex shader already outputs world xz in
// TEXCOORD0.zw, so the patched shader (ShaderPatches::PatchFloor) reads the world light atlas there
// (lightmap_smooth.cpp): brightest of the lot map and the ground light, like the snowy lot ground. ----
// ---- Fences, railings, posts, stairs (instanced lot structures, ShaderPatches::IsInstancedStructureVs). Their vertex
// shader takes lamp light only from the rig's "vertex light" arrays, which the game fills only with overflow lights
// (usually none: VS c4..c11 = 0 in every fence capture), and one rig serves a whole group from its centre. The patched
// pixel shader (ShaderPatches::PatchInstancedLamps) uses max(vertex lights, ground light atlas at the pixel) instead,
// so every rail gets the same lamp light as the ground next to it. ----
template <typename DrawFn> bool DrawInstanced(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_fencePs, "Fence/stairs", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchInstancedLamps(t, pp.inst); });
    if (!p.ps) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.inst.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.inst.strengthConst, oldB, 1);
    const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed), 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.inst.atlasSampler, atlas, D3DTEXF_NONE);
        dev->SetPixelShaderConstantF(p.inst.atlasConst, c, 1);
        dev->SetPixelShaderConstantF(p.inst.strengthConst, s, 1);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(original);
        dev->SetPixelShaderConstantF(p.inst.strengthConst, oldB, 1);
        dev->SetPixelShaderConstantF(p.inst.atlasConst, oldA, 1);
    }
    g_inOwnCall = false;
    g_fenceDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Snow lying on objects (fence tops, rails, props; ShaderPatches::IsSnowCoverVs). Its pixel shader lights the snow
// with the moon and the sky only, no lamp at all (LightProbe-neve-cerca, PS_2F27C510). The patched shader
// (ShaderPatches::PatchSnowCover) adds the ground light atlas at the pixel x the fence strength, like the ground does. ----
// ---- Snow with relief on stair tops (ShaderPatches::IsSnowReliefVs, LightProbe-neve-escada, PS_2E036820): same idea,
// patched by ShaderPatches::PatchSnowRelief; its world position arrives halved, so the atlas scale is doubled. ----
template <typename DrawFn, typename PatchFn>
bool DrawSnowOnObject(IDirect3DDevice9* dev, DrawFn draw, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>& cache, const char* what, PatchFn patch, float posScale,
                      std::atomic<int>& drawn) {
    if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    c[0] *= posScale;
    c[1] *= posScale;
    PatchedPs& p = PatchedFor(dev, cache, what, patch);
    if (!p.ps) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.snow.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.snow.strengthConst, oldB, 1);
    const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed), 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.snow.atlasSampler, atlas, D3DTEXF_NONE);
        dev->SetPixelShaderConstantF(p.snow.atlasConst, c, 1);
        dev->SetPixelShaderConstantF(p.snow.strengthConst, s, 1);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(original);
        dev->SetPixelShaderConstantF(p.snow.strengthConst, oldB, 1);
        dev->SetPixelShaderConstantF(p.snow.atlasConst, oldA, 1);
    }
    g_inOwnCall = false;
    drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}
template <typename DrawFn> bool DrawSnowCover(IDirect3DDevice9* dev, DrawFn draw) {
    return DrawSnowOnObject(dev, draw, g_snowCoverPs, "Snow on objects", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowCover(t, pp.snow); },
                            1.0f, g_snowCoverDrawn);
}
// ---- Outdoor objects lit by a rig (doors, windows, sofas, counters; LightProbe-porta/janela/sofa/balcao). The rig lights
// a whole object (or part) with the 3 strongest lamps at its centre, often from above at a grazing angle, while the walls
// around sum every lamp point by point: a front door looked dark next to its lit wall, and pieces of modular counters
// outside get different light. The patched shaders (ShaderPatches::PatchObjectLampVs / PatchObjectLampPs) use, per pixel,
// max(rig lamps + vertex lights, ground light atlas * (0.5 + 0.5 N.y) * strength). Only when the bound rig is outdoor
// (RigTracker: rig+0x1D4 == 2): indoor objects would pick up ground light from under the house. ----
IDirect3DVertexShader9* ObjectVsFor(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    auto it = g_objectVs.find(vs);
    if (it == g_objectVs.end()) return nullptr;
    FoliageVs& f = it->second;
    if (!f.tried) {
        f.tried = true;
        if (FAILED(dev->CreateVertexShader(f.code.data(), &f.vs))) f.vs = nullptr;
        LOG_INFO(std::format("[LotLightBridge] Outdoor object: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs), f.vs ? "patched" : "failed"));
    }
    return f.vs;
}

// Light probe detail (Ctrl+Shift+F7) for an object the mod lights: its position, the lamps it got and the game's own
// rig values before they are zeroed, so two neighbouring pieces can be compared. Only while a capture is recording.
std::string g_objDrawInfo;
std::string DescribeObjectDraw(IDirect3DDevice9* dev, const ShaderPatches::ObjectLampPatch& obj, int rigMode, bool pixelLamps, int wk, int vl, const float (*lamps)[4],
                               int nLamps) {
    auto v4 = [](const float* v) { return std::format("({:.4g} {:.4g} {:.4g} {:.4g})", v[0], v[1], v[2], v[3]); };
    std::string s = std::format("OBJECT fixed by the mod | {} | rig mode {} ({}) | per-pixel light {} (strength {:.2f}) | ground light strength {:.2f}",
                                obj.rigLamps ? "rig lamps in the PS (form A/B)" : "no lamps in the PS (form C)", rigMode,
                                rigMode == 2 ? "outdoors" : rigMode == 1 ? "roofless area" : "?", pixelLamps ? "on" : "OFF", g_objPixelLampStrength.load(),
                                g_objPixelStrength.load());
    float w[3][4] = {};
    if (wk >= 0) dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &w[0][0], 3);
    s += std::format("\n      object position (VS c{}..c{} .w) = ({:.2f} {:.2f} {:.2f})", wk, wk + 2, w[0][3], w[1][3], w[2][3]);
    s += std::format("\n      per-pixel lamps: {} used of {} in range", nLamps, g_lastLampCandidates);
    for (int k = 0; k < nLamps; k++) {
        const float *p = lamps[1 + 2 * k], *c = lamps[2 + 2 * k];
        const float r = p[3] > 0 ? 1.0f / std::sqrt(p[3]) : 0.0f, dx = p[0] - w[0][3], dy = p[1] - w[1][3], dz = p[2] - w[2][3];
        s += std::format("\n        [{}] pos ({:.2f} {:.2f} {:.2f}) radius {:.2f} colour ({:.3f} {:.3f} {:.3f}) distance to the object centre {:.2f}", k, p[0], p[1], p[2], r, c[0], c[1],
                         c[2], std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    float pc[14][4] = {};
    dev->GetPixelShaderConstantF(0, &pc[0][0], 14);
    s += "\n      the game's rig in the PS, before zeroing (c1..c3 direction and c5..c7 colour of the 3 lamps; sun in c8/c9, c0/c4 or c12/c13 depending on the variant):";
    for (int i = 0; i < 14; i++) s += std::format(" [{}]{}", i, v4(pc[i]));
    if (vl >= 4) {
        float v[8][4] = {};
        dev->GetVertexShaderConstantF(static_cast<UINT>(vl - 4), &v[0][0], 8);
        s += std::format("\n      luzes de vertice do jogo, antes de zerar (direcoes c{}..c{}, cores c{}..c{}):", vl - 4, vl - 1, vl, vl + 3);
        for (int i = 0; i < 8; i++) s += std::format(" [{}]{}", vl - 4 + i, v4(v[i]));
    } else
        s += "\n      vertex lights: base not found in the shader (not zeroed)";
    return s;
}

template <typename DrawFn> bool DrawObjectLamp(IDirect3DDevice9* dev, DrawFn draw) {
    // rig modes 2 (outdoors) and 1 (roofless fenced areas) both draw with the exterior technique (rig report 25/09)
    const int rigMode = RigTracker::CurrentMode();
    if (!g_objPixel.load(std::memory_order_relaxed) || (rigMode != 2 && rigMode != 1)) return false;
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_objLampPs, "Outdoor object (ground light)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchObjectLampPs(t, pp.obj); });
    if (!p.ps) return false;
    IDirect3DVertexShader9* vs = ObjectVsFor(dev, g_curVs);
    if (!vs) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.obj.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.obj.strengthConst, oldB, 1);
    const float s[4] = {g_objPixelStrength.load(std::memory_order_relaxed), 0, 0, 0};
    // Per-pixel lamps ("Counters" request): the same world lamps for every piece, chosen by the object's position (the
    // VS world triple's translation), so neighbouring pieces of a modular object get the same lamps.
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    float lamps[1 + 2 * N][4] = {};
    float oldLamps[1 + 2 * N][4] = {};
    const bool pixelLamps = g_objPixelLamps.load(std::memory_order_relaxed);
    bool zeroRig = false;
    float oldRig[3][4] = {}, oldVl[4][4] = {};
    const float noRig[4][4] = {};
    const int vl = g_objectVs[g_curVs].vertexLight, wk = g_objectVs[g_curVs].worldK;
    int nLamps = 0;
    lamps[0][3] = 1e-4f;
    for (unsigned k = 0; k < N; k++) lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f; // unused slot: far away, colour 0
    if (pixelLamps) {
        float m[3][4];
        if (wk >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &m[0][0], 3))) {
            const int n = SelectLamps(m[0][3], m[2][3], 40.0f);
            nLamps = std::min(n, static_cast<int>(N));
            for (int k = 0; k < n && k < static_cast<int>(N); k++) {
                const float* pr = g_lampData[k];
                const float* col = g_lampData[16 + k];
                const float r = pr[3] > 0.1f ? pr[3] : 0.1f;
                lamps[1 + 2 * k][0] = pr[0];
                lamps[1 + 2 * k][1] = pr[1];
                lamps[1 + 2 * k][2] = pr[2];
                lamps[1 + 2 * k][3] = 1.0f / (r * r);
                lamps[2 + 2 * k][0] = col[0];
                lamps[2 + 2 * k][1] = col[1];
                lamps[2 + 2 * k][2] = col[2];
            }
            // the rig goes: its 3 pixel lamps (PS c5..c7 = 0 below, diffuse and specular) and its 4 vertex lights (the VS
            // colour constants = 0; Phong's ambient term in COLOR0 stays)
            lamps[0][1] = g_objPixelLampStrength.load(std::memory_order_relaxed);
            zeroRig = true;
        }
    }
    if (LightProbe::Capturing()) g_objDrawInfo = DescribeObjectDraw(dev, p.obj, rigMode, zeroRig, wk, vl, lamps, nLamps);
    dev->GetPixelShaderConstantF(p.obj.lampParamConst, &oldLamps[0][0], 1 + 2 * N);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.obj.atlasSampler, atlas, D3DTEXF_NONE);
        dev->SetPixelShaderConstantF(p.obj.atlasConst, c, 1);
        dev->SetPixelShaderConstantF(p.obj.strengthConst, s, 1);
        dev->SetPixelShaderConstantF(p.obj.lampParamConst, &lamps[0][0], 1 + 2 * N);
        if (zeroRig && p.obj.rigLamps) {
            dev->GetPixelShaderConstantF(5, &oldRig[0][0], 3);
            dev->SetPixelShaderConstantF(5, &noRig[0][0], 3);
        }
        if (zeroRig && vl >= 0) {
            dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &oldVl[0][0], 4);
            dev->SetVertexShaderConstantF(static_cast<UINT>(vl), &noRig[0][0], 4);
        }
        dev->SetVertexShader(vs);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(originalPs);
        dev->SetVertexShader(originalVs);
        if (zeroRig && vl >= 0) dev->SetVertexShaderConstantF(static_cast<UINT>(vl), &oldVl[0][0], 4);
        if (zeroRig && p.obj.rigLamps) dev->SetPixelShaderConstantF(5, &oldRig[0][0], 3);
        dev->SetPixelShaderConstantF(p.obj.lampParamConst, &oldLamps[0][0], 1 + 2 * N);
        dev->SetPixelShaderConstantF(p.obj.strengthConst, oldB, 1);
        dev->SetPixelShaderConstantF(p.obj.atlasConst, oldA, 1);
    }
    g_objDrawInfo.clear();
    g_inOwnCall = false;
    g_objLampDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

template <typename DrawFn> bool DrawSnowRelief(IDirect3DDevice9* dev, DrawFn draw) {
    return DrawSnowOnObject(dev, draw, g_snowReliefPs, "Snow with relief (stairs)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowRelief(t, pp.snow); },
                            2.0f, g_snowReliefDrawn);
}

// Outdoor walls (wall_lamp_table.h): the baked lamp light's scale cK.x times "Forca nas paredes" for this draw.
std::atomic<int> g_wallDrawn{0};
template <typename DrawFn> bool DrawWallGain(IDirect3DDevice9* dev, DrawFn draw) {
    const float gain = g_wallGain.load(std::memory_order_relaxed);
    if (gain == 1.0f) return false;
    auto it = g_wallConst.find(g_curPs);
    if (it == g_wallConst.end()) return false;
    float c[4], old[4];
    if (FAILED(dev->GetPixelShaderConstantF(it->second, old, 1))) return false;
    std::memcpy(c, old, sizeof(c));
    c[0] *= gain;
    g_inOwnCall = true;
    dev->SetPixelShaderConstantF(it->second, c, 1);
    draw();
    dev->SetPixelShaderConstantF(it->second, old, 1);
    g_inOwnCall = false;
    g_wallDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

template <typename DrawFn> bool DrawFloor(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_floorPs, "Floor", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchFloor(t, pp.floor); });
    if (!p.ps) return false;
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        dev->SetPixelShaderConstantF(p.floor.atlasConst, c, 1);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(original);
        dev->SetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    }
    g_inOwnCall = false;
    g_floorDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Snow lying on lot floor tiles (LightProbe-m69): same max(room map, atlas) as the floors. Its TEXCOORD7 is world xz / 2,
// so the atlas scale is doubled.
template <typename DrawFn> bool DrawSnowFloor(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_curSnowFloorTc == 0 ? g_snowFloorPs0 : g_snowFloorPs, "Snow on floors", [tc = g_curSnowFloorTc](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowFloor(t, tc, pp.floor); });
    if (!p.ps) return false;
    // the map it lights with must be a room light map (A8R8G8B8 managed: 32x64 in m69 / m71, 512x128 in m72), not a terrain map (DXT5)
    bool roomMap = false;
    IDirect3DBaseTexture9* map = nullptr;
    if (SUCCEEDED(dev->GetTexture(p.floor.mapSampler, &map)) && map) {
        D3DSURFACE_DESC d{};
        if (map->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(map)->GetLevelDesc(0, &d)))
            roomMap = d.Format == D3DFMT_A8R8G8B8 && d.Pool != D3DPOOL_DEFAULT && d.Width <= 1024 && d.Height <= 1024;
        map->Release();
    }
    if (!roomMap) return false;
    const float half[4] = {c[0] * 2.0f, c[1] * 2.0f, c[2], c[3]};
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        dev->SetPixelShaderConstantF(p.floor.atlasConst, half, 1);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(original);
        dev->SetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    }
    g_inOwnCall = false;
    g_snowFloorDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Outdoor floors lit only by their baked floor map (summer; floor_atlas_table.h): the vertex shader gets a copy that
// also writes world xz to the first free TEXCOORD (7 or up: many floor vertex shaders already use 7), the pixel shader
// a copy with max(floor map, atlas) before the map's scale (ShaderPatches::PatchBakedAtlasPs), as the winter floors.
struct FloorVs {
    IDirect3DVertexShader9* vs = nullptr;
    bool tried = false;
    int tc = 7;
};
std::unordered_map<IDirect3DVertexShader9*, FloorVs> g_floorVs;
std::map<int, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>> g_floorAtlasPs; // per TEXCOORD index
std::atomic<int> g_floorAtlasDrawn{0};

template <typename DrawFn> bool DrawFloorAtlas(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas || !g_curVs) return false;
    FloorVs& fv = g_floorVs[g_curVs];
    if (!fv.tried) {
        fv.tried = true;
        UINT size = 0;
        if (SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
            std::vector<DWORD> t(size / 4);
            int tc = -1;
            if (SUCCEEDED(g_curVs->GetFunction(t.data(), &size)) && ShaderPatches::PatchObjectLampVs(t, false, &tc) && SUCCEEDED(dev->CreateVertexShader(t.data(), &fv.vs)))
                fv.tc = tc;
            else
                fv.vs = nullptr;
        }
        LOG_INFO(std::format("[LotLightBridge] Outdoor floor: vertex shader {:08X} {} (TEXCOORD{})", reinterpret_cast<uintptr_t>(g_curVs), fv.vs ? "patched" : "without the expected pattern", fv.tc));
    }
    if (!fv.vs) return false;
    PatchedPs& p = PatchedFor(dev, g_floorAtlasPs[fv.tc], "Outdoor floor", [tc = fv.tc](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBakedAtlasPs(t, tc, pp.floor); });
    if (!p.ps) return false;
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        dev->SetPixelShaderConstantF(p.floor.atlasConst, c, 1);
        dev->SetVertexShader(fv.vs);
        dev->SetPixelShader(p.ps);
        draw();
        dev->SetPixelShader(originalPs);
        dev->SetVertexShader(originalVs);
        dev->SetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    }
    g_inOwnCall = false;
    g_floorAtlasDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Foliage. Trees and bushes (summer and winter) are lit per vertex: sun + 3 lamps per instance, N.L clamped at 0
// ("max r0, r0, cK.w"), so the side of a bush away from a lamp is black. Patched vertex shader (ShaderPatches::
// PatchFoliageVs): lamps get wrap lighting. Winter bushes also multiply the lamp light by the moon shadow (like the
// summer object shader fixed in DrawObjectRig): patched pixel shader (ShaderPatches::PatchLeafShadow) lifts that shadow
// to 1 at night. ----
IDirect3DVertexShader9* FoliageVsFor(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    auto it = g_foliageVs.find(vs);
    if (it == g_foliageVs.end()) return nullptr;
    FoliageVs& f = it->second;
    if (!f.tried) {
        f.tried = true;
        if (FAILED(dev->CreateVertexShader(f.code.data(), &f.vs))) f.vs = nullptr;
        LOG_INFO(std::format("[LotLightBridge] Foliage: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs), f.vs ? "patched" : "failed"));
    }
    return f.vs;
}

template <typename DrawFn> bool DrawLeafShadow(IDirect3DDevice9* dev, DrawFn draw) {
    const float night = g_night.load(std::memory_order_relaxed);
    if (!g_objectFix.load(std::memory_order_relaxed) || night <= 0.01f) return false;
    PatchedPs& p = PatchedFor(dev, g_leafPs, "Foliage (moon shadow)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchLeafShadow(t, pp.nightConst); });
    if (!p.ps) return false;
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.nightConst, oldC, 1);
    const float c[4] = {night, 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    dev->SetPixelShaderConstantF(p.nightConst, c, 1);
    dev->SetPixelShader(p.ps);
    draw();
    dev->SetPixelShader(original);
    dev->SetPixelShaderConstantF(p.nightConst, oldC, 1);
    g_inOwnCall = false;
    g_leafDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Snowy roofs (shader_ids.h kRoofSnowPs): the winter roof shader has no lamp light and is too big to
// rewrite (snow relief noise). After the game draws it, an additive pass (roof_snow_lamps_ps.hlsl) on the same geometry
// adds the nearby lamps on the same snowy albedo. ----
IDirect3DPixelShader9* g_roofSnowPs = nullptr;
bool g_roofSnowTried = false;

template <typename DrawFn> bool DrawRoofSnow(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_roofFix.load(std::memory_order_relaxed)) return false;
    if (!g_roofSnowPs && !g_roofSnowTried) {
        g_roofSnowTried = true;
        const std::string err = CompilePs(dev, kRoofSnowPsId, &g_roofSnowPs);
        LOG_INFO("[LotLightBridge] Snowy roofs: " + (err.empty() ? std::string("active") : err));
    }
    if (!g_roofSnowPs) return false;
    float world[12], vs15[4];
    if (FAILED(dev->GetVertexShaderConstantF(8, world, 3)) || FAILED(dev->GetVertexShaderConstantF(15, vs15, 1))) return false;
    g_inOwnCall = true;
    draw(); // the game's roof, unchanged
    const int picked = SelectLamps(world[3], world[11], 80.0f);
    if (picked > 0 && std::fabs(vs15[0]) > 1e-6f) {
        float consts[34][4] = {};
        std::memcpy(consts, g_lampData, sizeof(float) * 4 * 33);
        consts[32][0] = g_roofStrength.load(std::memory_order_relaxed);
        consts[32][1] = static_cast<float>(picked);
        consts[33][0] = vs15[0]; // COLOR0 = world xz / VS c15.x
        float saved[34][4];
        dev->GetPixelShaderConstantF(20, &saved[0][0], 34);
        constexpr D3DRENDERSTATETYPE kRs[8] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE};
        const DWORD set[8] = {TRUE, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, FALSE, FALSE, 0x7, FALSE};
        DWORD rs[8];
        for (int i = 0; i < 8; i++) dev->GetRenderState(kRs[i], &rs[i]);
        for (int i = 0; i < 8; i++) dev->SetRenderState(kRs[i], set[i]);
        IDirect3DPixelShader9* original = g_curPs;
        dev->SetPixelShader(g_roofSnowPs);
        dev->SetPixelShaderConstantF(20, &consts[0][0], 34);
        draw();
        dev->SetPixelShaderConstantF(20, &saved[0][0], 34);
        dev->SetPixelShader(original);
        for (int i = 7; i >= 0; i--) dev->SetRenderState(kRs[i], rs[i]);
        g_roofSnowDrawn.fetch_add(1, std::memory_order_relaxed);
    }
    g_inOwnCall = false;
    return true;
}

// Draws an outdoor object with the moon-shadow-free shader (c3.x = night level), then restores the game state.
template <typename DrawFn> bool DrawObjectRig(IDirect3DDevice9* dev, DrawFn draw) {
    const float night = g_night.load(std::memory_order_relaxed);
    if (!g_objectFix.load(std::memory_order_relaxed) || night <= 0.01f) return false;
    EnsureObjectReplacement(dev);
    if (!g_objectPs) return false;
    float oldC3[4] = {};
    dev->GetPixelShaderConstantF(3, oldC3, 1);
    const float c3[4] = {night, 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    dev->SetPixelShader(g_objectPs);
    dev->SetPixelShaderConstantF(3, c3, 1);
    draw();
    dev->SetPixelShaderConstantF(3, oldC3, 1);
    dev->SetPixelShader(original);
    g_inOwnCall = false;
    g_objectDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Returns the sampler that holds the chunk light map (0 = not a terrain chunk draw) and the chunk key.
DWORD RecordWorldChunk(IDirect3DDevice9* dev, std::pair<int, int>& key) {
    float c[16];
    if (FAILED(dev->GetVertexShaderConstantF(8, c, 3))) return 0; // c8..c10 = world matrix rows
    float m[4];
    if (FAILED(dev->GetVertexShaderConstantF(15, m, 1)) || !Near(m[0], 1.0f / 256.0f) || !Near(m[1], 1.0f / 256.0f) || !Near(m[2], 0.5f) || !Near(m[3], 0.5f)) return 0;
    // The terrain light map is the 256x256 texture with few mips (4 when baked, 1 when rebuilt). Which sampler holds it
    // depends on the shader variant (s8 with 4 paint layers, s7 with 3, ...). The normal map is also 256x256 but has 9
    // mips and format Q8W8V8U8, the paint layers are 1024x1024.
    IDirect3DBaseTexture9* t = nullptr;
    DWORD sampler = 0;
    for (DWORD s = 15; s >= 1 && !t; s--) {
        IDirect3DBaseTexture9* cand = nullptr;
        if (FAILED(dev->GetTexture(s, &cand)) || !cand) continue;
        bool ok = cand->GetType() == D3DRTYPE_TEXTURE && cand->GetLevelCount() <= 5;
        if (ok) {
            D3DSURFACE_DESC d{};
            ok = SUCCEEDED(static_cast<IDirect3DTexture9*>(cand)->GetLevelDesc(0, &d)) && d.Width == 256 && d.Height == 256 && d.Format != D3DFMT_Q8W8V8U8;
        }
        if (ok) {
            t = cand;
            sampler = s;
        }
        else cand->Release();
    }
    if (!t) return 0;
    key = Key(c[3], c[11]);
    auto& slot = g_chunks[key];
    if (slot.tex != t) {
        if (slot.tex) slot.tex->Release();
        slot.tex = t; // keep the reference from GetTexture
    } else
        t->Release();
    g_worldSeen.fetch_add(1, std::memory_order_relaxed);
    return sampler;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDrawInner(IDirect3DDevice9* dev, DrawFn draw) {
    constexpr auto kSkip = D3D9Hooks::HookAction::Skip;
    constexpr auto kContinue = D3D9Hooks::HookAction::Continue;
    if (g_curClass == PsClass::ObjectRig) return DrawObjectRig(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::Roof) return DrawRoof(dev, draw) ? kSkip : kContinue;
    // The snowy roof pixel shader is also the one of snow on stair tops (same bytes, LightProbe-neve-escada); the vertex
    // shader tells them apart (the stair one takes the snow base in TEXCOORD2) and stairs go to DrawSnowRelief below.
    if (g_curClass == PsClass::RoofSnow && !g_curVsIsSnowRelief) return DrawRoofSnow(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::Lake) return DrawLake(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsFoliage) return DrawLeafShadow(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::WallGain) return DrawWallGain(dev, draw) ? kSkip : kContinue;
    if (!g_enabled.load(std::memory_order_relaxed)) return kContinue;
    if (g_curVsIsRoad) return DrawRoad(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsFloor) return DrawFloor(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::FloorAtlas && !g_curVsIsSnowFloor) return DrawFloorAtlas(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsInstanced) return DrawInstanced(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsSnowCover) return DrawSnowCover(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsSnowRelief) return DrawSnowRelief(dev, draw) ? kSkip : kContinue;
    // Class 10 also holds roof and snow vertex shaders: when the object patch does not apply, fall through to the rest.
    if (g_curVsIsObject && DrawObjectLamp(dev, draw)) return kSkip;
    if (g_curClass == PsClass::LotLightSnow) return DrawLotSnow(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::WorldCandidate) {
        std::pair<int, int> key;
        const DWORD s = RecordWorldChunk(dev, key);
        if (!s) {
            // not a world terrain chunk: the snow-on-floor pixel shaders (m69, m71) also declare s6+ and land here
            if (g_curVsIsSnowFloor && DrawSnowFloor(dev, draw)) return kSkip;
            return D3D9Hooks::HookAction::Continue;
        }
        IDirect3DTexture9* smooth = LightmapSmooth::Get(key, static_cast<IDirect3DTexture9*>(g_chunks[key].tex));
        if (!smooth) return D3D9Hooks::HookAction::Continue;
        IDirect3DBaseTexture9* old = nullptr;
        dev->GetTexture(s, &old);
        g_inOwnCall = true;
        dev->SetTexture(s, smooth);
        draw();
        dev->SetTexture(s, old);
        g_inOwnCall = false;
        if (old) old->Release();
        return D3D9Hooks::HookAction::Skip;
    }
    if (g_curClass != PsClass::LotLight) {
        // snow lying on floor tiles (m69, m71): only draws no pixel-shader class claimed, see ClassifyVs
        if (g_curVsIsSnowFloor && DrawSnowFloor(dev, draw)) return kSkip;
        return kContinue;
    }

    EnsureReplacement(dev);
    if (!g_replacementPs) return D3D9Hooks::HookAction::Continue;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(14, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return D3D9Hooks::HookAction::Continue;
    // World atlas when ready, as in DrawLotSnow (a lot can reach past its home chunk). The summer lot VS computes the
    // light map uv as (world.xz - c15.xz) * c14.xy + c14.zw; c15 also feeds another uv (c13), so only c14 changes.
    float atlasC[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(atlasC);
    IDirect3DBaseTexture9* terrain = atlas;
    if (!atlas) {
        auto it = g_chunks.find(Key(v[4], v[6])); // c15.xz = chunk center
        if (it == g_chunks.end() || !it->second.tex) {
            g_lotMissing.fetch_add(1, std::memory_order_relaxed);
            return D3D9Hooks::HookAction::Continue;
        }
        terrain = ChunkTexture(it->first, it->second.tex);
    }
    const float atlasMap[4] = {atlasC[0], atlasC[1], atlasC[2] + v[4] * atlasC[0], atlasC[3] + v[6] * atlasC[1]};

    // Soft lot edges: PS c28..c30 (see kReplacementHlsl). Without a known lot rectangle the pass is the plain max().
    float edge[12];
    float lotM[12] = {};
    const LotRect* rect = nullptr;
    if (g_softEdges.load(std::memory_order_relaxed) && SUCCEEDED(dev->GetVertexShaderConstantF(8, lotM, 3))) {
        rect = FindLotRect(&lotM[0], &lotM[8]);
        if (!rect) g_lotRectMiss = true;
    }
    const bool feather = LotEdgeConstants(atlas ? atlasMap : v, &v[4], &lotM[0], &lotM[8], rect, edge);
    if (feather) {
        g_edgeMatched.fetch_add(1, std::memory_order_relaxed);
        g_lastEdgeRect = *rect;
        g_haveLastEdgeRect = true;
    } else if (g_softEdges.load(std::memory_order_relaxed))
        g_edgeUnmatched.fetch_add(1, std::memory_order_relaxed);
    if (LightProbe::Capturing())
        g_objDrawInfo = feather ? std::format("mod draw: lot light pass | soft edges: lot {:08X}{:08X}, {:.0f} x {:.0f} m, origin ({:.2f}, {:.2f}), band {:.1f} m (PS c28..c30)",
                                              rect->lotHi, rect->lotLo, rect->w, rect->d, rect->tx, rect->tz, kEdgeBand)
                                : std::string("mod draw: lot light pass | soft edges: ") + (g_softEdges.load() ? "NOT applied, lot rectangle not found" : "off (option)");
    float savedEdge[12];
    dev->GetPixelShaderConstantF(28, savedEdge, 3);

    IDirect3DPixelShader9* original = g_curPs;
    IDirect3DBaseTexture9* old2 = nullptr;
    dev->GetTexture(2, &old2);
    DWORD au, av, mn, mg, mp, srgb;
    dev->GetSamplerState(2, D3DSAMP_ADDRESSU, &au);
    dev->GetSamplerState(2, D3DSAMP_ADDRESSV, &av);
    dev->GetSamplerState(2, D3DSAMP_MINFILTER, &mn);
    dev->GetSamplerState(2, D3DSAMP_MAGFILTER, &mg);
    dev->GetSamplerState(2, D3DSAMP_MIPFILTER, &mp);
    dev->GetSamplerState(2, D3DSAMP_SRGBTEXTURE, &srgb);

    g_inOwnCall = true;
    dev->SetPixelShader(g_replacementPs);
    dev->SetTexture(2, terrain);
    if (atlas) dev->SetVertexShaderConstantF(14, atlasMap, 1);
    dev->SetPixelShaderConstantF(28, edge, 3);
    dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE);
    draw();
    dev->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, srgb);
    dev->SetSamplerState(2, D3DSAMP_MIPFILTER, mp);
    dev->SetSamplerState(2, D3DSAMP_MAGFILTER, mg);
    dev->SetSamplerState(2, D3DSAMP_MINFILTER, mn);
    dev->SetSamplerState(2, D3DSAMP_ADDRESSV, av);
    dev->SetSamplerState(2, D3DSAMP_ADDRESSU, au);
    dev->SetTexture(2, old2);
    if (old2) old2->Release();
    if (atlas) dev->SetVertexShaderConstantF(14, v, 1);
    dev->SetPixelShaderConstantF(28, savedEdge, 3);
    g_objDrawInfo.clear();
    dev->SetPixelShader(original);
    g_inOwnCall = false;
    g_lotDrawn.fetch_add(1, std::memory_order_relaxed);
    return D3D9Hooks::HookAction::Skip;
}

// Foliage vertex shaders are swapped around everything else (the pixel side may be patched too): set the patched VS,
// let the pixel-shader handling draw (or draw here), restore the game's VS.
void TrackPs(IDirect3DPixelShader9* ps) {
    g_curPs = ps;
    g_curClass = Classify(ps);
}

void TrackVs(IDirect3DVertexShader9* vs) {
    g_curVs = vs;
    const uint8_t cls = ClassifyVs(vs);
    g_curVsIsRoof = cls == 1;
    g_curVsIsLake = cls == 2;
    g_curVsIsSnowLot = cls == 3;
    g_curVsIsRoad = cls == 4;
    if (g_curVsIsRoad) g_curRoadMap = g_roadMapConst[vs];
    g_curVsIsFloor = cls == 5;
    g_curVsIsFoliage = cls == 6;
    g_curVsIsInstanced = cls == 7;
    g_curVsIsSnowCover = cls == 8;
    g_curVsIsSnowRelief = cls == 9;
    g_curVsIsObject = cls == 10;
    g_curVsIsSnowFloor = cls == 11;
    if (g_curVsIsSnowFloor) g_curSnowFloorTc = g_snowFloorTc[vs];
}

template <typename DrawFn> D3D9Hooks::HookAction OnDrawTracked(IDirect3DDevice9* dev, DrawFn draw) {
    if (g_inOwnCall) return D3D9Hooks::HookAction::Continue;
    if (g_stateUnknown) {
        // hooks registered mid-session (or after Shutdown): the shaders bound now never went through our Set*Shader
        // hooks, so read them from the device once (review 25/09)
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        dev->GetPixelShader(&ps);
        dev->GetVertexShader(&vs);
        TrackPs(ps);
        TrackVs(vs);
        if (ps) ps->Release(); // Classify / ClassifyVs pinned them
        if (vs) vs->Release();
        g_stateUnknown = false;
    }
    IDirect3DVertexShader9* foliage = g_curVsIsFoliage && g_objectFix.load(std::memory_order_relaxed) ? FoliageVsFor(dev, g_curVs) : nullptr;
    if (!foliage) return OnDrawInner(dev, draw);
    IDirect3DVertexShader9* original = g_curVs;
    g_inOwnCall = true;
    dev->SetVertexShader(foliage);
    g_inOwnCall = false;
    if (OnDrawInner(dev, draw) == D3D9Hooks::HookAction::Continue) {
        g_inOwnCall = true;
        draw();
        g_inOwnCall = false;
    }
    g_inOwnCall = true;
    dev->SetVertexShader(original);
    g_inOwnCall = false;
    g_foliageDrawn.fetch_add(1, std::memory_order_relaxed);
    return D3D9Hooks::HookAction::Skip;
}

// ---- Census and false colour (development build). A draw is a "lamp-lit candidate" when it binds a baked light map
// (A8R8G8B8 managed, one level, up to 1024: room / wall / floor / lot maps; or a 256x256 DXT5 terrain map) or is drawn
// with an outdoor rig (RigTracker mode 2). Candidates no fix claimed (OnDrawTracked returned Continue) are painted
// magenta in false-colour mode, and the census records them per (VS, PS) pair for ApexRadiance_Censo.txt. ----
UINT g_curPrims = 0;
std::atomic<bool> g_falseColor{false};
std::atomic<int> g_censusFrames{0}; // frames left to record
bool g_censusPending = false;       // write the report when the frames are done
struct CensusRow {
    int draws = 0, claimed = 0;
    UINT prims = 0;
    int rig = -1;
    std::string tex;
};
std::map<std::pair<IDirect3DVertexShader9*, IDirect3DPixelShader9*>, CensusRow> g_census;
std::unordered_map<IDirect3DPixelShader9*, bool> g_psIs3;
IDirect3DPixelShader9* g_magenta[2] = {}; // ps_2_0, ps_3_0
bool g_magentaTried = false;

bool LitCandidate(IDirect3DDevice9* dev, std::string& desc) {
    bool lit = false;
    for (DWORD s = 0; s < 16; s++) {
        IDirect3DBaseTexture9* b = nullptr;
        if (FAILED(dev->GetTexture(s, &b)) || !b) continue;
        D3DSURFACE_DESC d{};
        if (b->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(b)->GetLevelDesc(0, &d))) {
            const DWORD levels = b->GetLevelCount();
            if (d.Format == D3DFMT_A8R8G8B8 && d.Pool != D3DPOOL_DEFAULT && levels == 1 && d.Width <= 1024 && d.Height <= 1024) {
                lit = true;
                desc += std::format(" s{}:map{}x{}", s, d.Width, d.Height);
            } else if (d.Format == D3DFMT_DXT5 && d.Width == 256 && d.Height == 256 && levels <= 5) {
                lit = true;
                desc += std::format(" s{}:terrain", s);
            }
        }
        b->Release();
    }
    return lit;
}

bool PsIs3(IDirect3DPixelShader9* ps) {
    if (!ps) return false;
    auto it = g_psIs3.find(ps);
    if (it != g_psIs3.end()) return it->second;
    DWORD tok = 0;
    UINT size = 0;
    bool is3 = false;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8) {
        std::vector<DWORD> t(size / 4);
        if (SUCCEEDED(ps->GetFunction(t.data(), &size))) tok = t[0];
        is3 = (tok & 0xFF00) == 0x0300;
    }
    g_psIs3[ps] = is3;
    return is3;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDraw(IDirect3DDevice9* dev, DrawFn draw) {
    const bool fc = g_falseColor.load(std::memory_order_relaxed), census = g_censusFrames.load(std::memory_order_relaxed) > 0;
    if (kPublicBuild || (!fc && !census) || g_inOwnCall) return OnDrawTracked(dev, draw);
    std::string desc;
    const int rig = RigTracker::CurrentMode();
    const bool candidate = LitCandidate(dev, desc) || rig == 2;
    const D3D9Hooks::HookAction r = OnDrawTracked(dev, draw);
    if (!candidate) return r;
    const bool claimed = r == D3D9Hooks::HookAction::Skip;
    if (census) {
        CensusRow& row = g_census[{g_curVs, g_curPs}];
        row.draws++;
        row.claimed += claimed ? 1 : 0;
        row.prims += g_curPrims;
        row.rig = rig;
        if (row.tex.empty()) row.tex = desc.empty() ? " (so rig)" : desc;
    }
    if (claimed || !fc || !g_curPs) return r;
    if (!g_magentaTried) {
        g_magentaTried = true;
        for (int v = 0; v < 2; v++) {
            const DWORD code[] = {v ? 0xFFFF0300u : 0xFFFF0200u, 0x05000051u, 0xA00F0000u, 0x3F800000u, 0u, 0x3F800000u, 0x3F800000u, // def c0, 1, 0, 1, 1
                                  0x02000001u, 0x800F0800u, 0xA0E40000u,                                                           // mov oC0, c0
                                  0x0000FFFFu};
            if (FAILED(dev->CreatePixelShader(code, &g_magenta[v]))) g_magenta[v] = nullptr;
        }
    }
    IDirect3DPixelShader9* m = g_magenta[PsIs3(g_curPs) ? 1 : 0];
    if (!m) return r;
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    dev->SetPixelShader(m);
    draw();
    dev->SetPixelShader(original);
    g_inOwnCall = false;
    return D3D9Hooks::HookAction::Skip;
}

void WriteCensus() {
    try {
        const std::filesystem::path base = std::filesystem::path(ApexPaths::ApexDirectory());
        const std::filesystem::path dir = base / L"Censo";
        std::filesystem::create_directories(dir);
        std::ofstream out(base / L"ApexRadiance_Censo.txt", std::ios::trunc);
        out << APEX_PRODUCT_NAME " census: draws that get baked light (light map) or an outdoor rig, per shader pair\n"
               "columns: draws | fixed | triangles | rig | VS hash/size | PS hash/size | textures\n\n";
        auto code = [](auto* sh, uint32_t& hash, UINT& size) {
            std::vector<DWORD> t;
            size = 0;
            hash = 0;
            if (sh && SUCCEEDED(sh->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
                t.resize(size / 4);
                if (FAILED(sh->GetFunction(t.data(), &size))) t.clear();
            }
            uint32_t h = 2166136261u;
            for (DWORD d : t) h = (h ^ d) * 16777619u;
            hash = t.empty() ? 0 : h;
            return t;
        };
        int unclaimed = 0;
        for (auto& [key, row] : g_census) {
            uint32_t vh, ph;
            UINT vsz, psz;
            const auto vs = code(key.first, vh, vsz);
            const auto ps = code(key.second, ph, psz);
            const bool none = row.claimed == 0;
            unclaimed += none ? 1 : 0;
            out << std::format("{} {:5} | {:5} | {:7} | {:2} | VS {:08X}/{} | PS {:08X}/{} |{}\n", none ? "SEM" : "ok ", row.draws, row.claimed, row.prims, row.rig, vh, vsz, ph,
                               psz, row.tex);
            if (none) {
                if (!vs.empty()) std::ofstream(dir / std::format("VS_{:08X}.bin", vh), std::ios::binary).write(reinterpret_cast<const char*>(vs.data()), vsz);
                if (!ps.empty()) std::ofstream(dir / std::format("PS_{:08X}.bin", ph), std::ios::binary).write(reinterpret_cast<const char*>(ps.data()), psz);
            }
        }
        out << std::format("\n{} pairs, {} without a fix (code in Apex Radiance\\Censo)\n", g_census.size(), unclaimed);
        LOG_INFO(std::format("[LotLightBridge] Census written: {} pairs, {} without a fix", g_census.size(), unclaimed));
    } catch (...) {
    }
    g_census.clear();
}

bool g_keepChunks = false; // Shutdown(true): a reinstall keeps the chunk maps, smoothed maps and atlas (same world)

void ClearChunks() {
    for (auto& [k, v] : g_chunks)
        if (v.tex) v.tex->Release();
    g_chunks.clear();
    LightmapSmooth::Clear();
}

} // namespace

namespace LotLightBridge {

// A C++ exception inside a hook would unwind into the game: turn the whole bridge off instead (review 25/09).
void HookFailed() {
    g_hookFailed = true;
    g_status = "Off after an internal error (see ApexRadiance_LOG.txt)";
    LOG_ERROR("[LotLightBridge] Exception inside the draw hook: fixes off until the game restarts");
}

void UpdateHooks() {
    static std::mutex m; // Install runs off the render thread at startup while Present may call the setters
    std::lock_guard<std::mutex> lock(m);
    const bool on = !g_hookFailed && (g_enabled.load() || g_objectFix.load() || g_roofFix.load() || g_waterFix.load() || g_wallGain.load() != 1.0f);
    if (on && !g_hooksRegistered) {
        g_stateUnknown = true;
        D3D9Hooks::RegisterSetPixelShader(kHookName, [](D3D9Hooks::DeviceContext&, IDirect3DPixelShader9* ps) {
            if (!g_inOwnCall && !g_hookFailed) try {
                    TrackPs(ps);
                } catch (...) {
                    HookFailed();
                }
            return D3D9Hooks::HookAction::Continue;
        });
        D3D9Hooks::RegisterSetVertexShader(kHookName, [](D3D9Hooks::DeviceContext&, IDirect3DVertexShader9* vs) {
            if (!g_inOwnCall && !g_hookFailed) try {
                    TrackVs(vs);
                } catch (...) {
                    HookFailed();
                }
            return D3D9Hooks::HookAction::Continue;
        });
        D3D9Hooks::RegisterDrawIndexedPrimitive(kHookName,
            [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, INT bvi, UINT minV, UINT numV, UINT start, UINT prims) {
                if (g_hookFailed) return D3D9Hooks::HookAction::Continue;
                try {
                    g_curPrims = prims;
                    return OnDraw(ctx.device, [&]() { ctx.device->DrawIndexedPrimitive(type, bvi, minV, numV, start, prims); });
                } catch (...) {
                    g_inOwnCall = false;
                    HookFailed();
                    return D3D9Hooks::HookAction::Continue;
                }
            });
        D3D9Hooks::RegisterDrawPrimitive(kHookName, [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
            if (g_hookFailed) return D3D9Hooks::HookAction::Continue;
            try {
                g_curPrims = prims;
                return OnDraw(ctx.device, [&]() { ctx.device->DrawPrimitive(type, start, prims); });
            } catch (...) {
                g_inOwnCall = false;
                HookFailed();
                return D3D9Hooks::HookAction::Continue;
            }
        });
        g_hooksRegistered = true;
        if (g_status == "Off") g_status = "Waiting for the first draw";
    } else if (!on && g_hooksRegistered) {
        D3D9Hooks::UnregisterAll(kHookName);
        g_hooksRegistered = false;
        if (!g_keepChunks) ClearChunks();
        g_status = "Off";
    }
}

void SetEnabled(bool on) {
    g_enabled = on;
    if (!on) {
        if (!g_keepChunks) ClearChunks();
        g_status = "Off";
    }
    UpdateHooks();
}

void SetObjectShadowFix(bool on) {
    g_objectFix = on;
    UpdateHooks();
}

void SetNightLevel(float level) { g_night = level < 0 ? 0.0f : (level > 1 ? 1.0f : level); }

void SetRoofFix(bool on, float strength) {
    g_roofStrength = strength;
    if (g_roofFix.load() != on) {
        g_roofFix = on;
        UpdateHooks();
    }
}

void SetWaterFix(bool on, float strength, float reflection) {
    g_waterStrength = strength;
    g_waterRefl = reflection;
    if (g_waterFix.load() != on) {
        g_waterFix = on;
        UpdateHooks();
    }
}

void SetSidewalkClear(float amount) { g_sidewalkClear = amount < 0 ? 0.0f : (amount > 1 ? 1.0f : amount); }

void OnWorldChanged() {
    ClearChunks();
    g_lotLampSig.clear(); // the next enumeration starts the new world's lots from scratch (all new: nothing counted)
    g_lotSeen.clear();
    g_quietLogAt.clear();
    g_bakeSnap = LotLightBridge::BakeSnapshot{};
    g_lastUserLots.clear();
    g_lotRects.clear(); // soft lot edges: the next Present reads the new world's lots
    g_lotRectMiss = true;
    g_haveLastEdgeRect = false;
}

void SetSoftLotEdges(bool on) { g_softEdges = on; }

std::string LotEdgeStatus() {
    if (!g_softEdges.load()) return "off";
    std::string s = std::format("on, band {:.1f} m | lots known: {} | lot passes feathered: {} | without a lot rectangle: {}", kEdgeBand, g_lotRects.size(),
                                g_edgeMatched.load(), g_edgeUnmatched.load());
    if (g_haveLastEdgeRect)
        s += std::format(" | last: lot {:08X}{:08X} {:.0f} x {:.0f} m at ({:.1f}, {:.1f})", g_lastEdgeRect.lotHi, g_lastEdgeRect.lotLo, g_lastEdgeRect.w, g_lastEdgeRect.d,
                         g_lastEdgeRect.tx, g_lastEdgeRect.tz);
    return s;
}

int ChunkCount() { return static_cast<int>(g_chunks.size()); }

void SetObjectPixelLamps(bool on, float strength) {
    g_objPixel = on;
    g_objPixelStrength = strength;
}

void SetObjectPixelLights(bool on, float strength) {
    g_objPixelLamps = on;
    g_objPixelLampStrength = strength;
}

void SetFenceGroundLight(bool on, float strength) {
    g_fenceFix = on;
    g_fenceStrength = strength;
}

void SetWallGain(float gain) {
    gain = gain < 0.25f ? 0.25f : (gain > 8.0f ? 8.0f : gain);
    const bool was = g_wallGain.load() != 1.0f;
    g_wallGain = gain;
    if (was != (gain != 1.0f)) UpdateHooks();
}

std::string WallStatus() { return std::format("outside walls: strength {:.2f} | draws: {} | variants seen: {}", g_wallGain.load(), g_wallDrawn.load(), g_wallConst.size()); }

std::string WaterStatus() {
    return std::format("water: {} | draws with reflection: {}", g_waterFix.load() ? (g_waterPs ? "active" : "waiting") : "off", g_waterDrawn.load());
}

void SetFalseColor(bool on) { g_falseColor = on; }

void RequestCensus() {
    if (g_censusPending) return;
    g_census.clear();
    g_censusPending = true;
    g_censusFrames = 3;
}

std::string CensusStatus() { return g_censusPending ? "writing..." : "ready"; }

void OnPresent() {
    if (g_censusPending && g_censusFrames.load() > 0 && --g_censusFrames == 0) {
        WriteCensus();
        g_censusPending = false;
    }
    // Lot rectangles for the soft lot edges: every 20 frames, or 5 frames after a lot pass found none (a lot that
    // streamed in) so a new lot gets its feather within a few frames.
    if (g_softEdges.load(std::memory_order_relaxed) && g_enabled.load(std::memory_order_relaxed)) {
        ++g_lotRectFrame;
        if (g_lotRectFrame >= 20 || (g_lotRectMiss && g_lotRectFrame >= 5)) {
            g_lotRectFrame = 0;
            g_lotRectMiss = false;
            RefreshLotRects();
        }
    }
    if (++g_lampFrame < 20 && !g_lampRefreshNow) return;
    g_lampFrame = 0;
    g_lampRefreshNow = false;
    if (g_roofFix.load() || g_waterFix.load() || g_objPixelLamps.load()) UpdateLampList(); // enumerates the lights
    else if (!EnumerateLights()) return;
    TrackLotLampEdits();
}

int LotLampEdits() { return g_lotLampEdits.load(std::memory_order_relaxed); }
int LotLampUserEdits() { return g_lotLampUserEdits.load(std::memory_order_relaxed); }

std::string LotLampStatus() {
    return std::format("changes counted: {} (user-driven: {}) | ignored (streaming, still loading, bulk): {} | not counted: outside the bake {}, below the threshold {}, "
                       "animated {} ({} lamps) | lots tracked: {} | last: {}",
                       g_lotChangesCounted, g_lotLampUserEdits.load(), g_lotChangesIgnored, g_lampChangesOutside, g_lampChangesNoise, g_lampChangesAnimated,
                       g_lampsAnimated, g_lotSeen.size(), g_lastLotChange);
}

const BakeSnapshot& CurrentBakeLamps() { return g_bakeSnap; }
int LampEnumerations() { return g_lampEnumerations; }
const std::vector<uint64_t>& LastUserChangeLots() { return g_lastUserLots; }
void RequestLampRefresh() { g_lampRefreshNow = true; }

// Lamps of `lot` in a snapshot sorted by lot: [first, last)
static std::pair<std::vector<BakeLamp>::const_iterator, std::vector<BakeLamp>::const_iterator> LotLamps(const BakeSnapshot& s, uint64_t lot) {
    const auto first = std::lower_bound(s.lamps.begin(), s.lamps.end(), lot, [](const BakeLamp& b, uint64_t v) { return b.lot < v; });
    const auto last = std::upper_bound(first, s.lamps.end(), lot, [](uint64_t v, const BakeLamp& b) { return v < b.lot; });
    return {first, last};
}

BakeDiff DiffBake(const BakeSnapshot& baked, const BakeSnapshot& now, bool plainLamps) {
    BakeDiff d;
    auto inBake = [plainLamps](const BakeLamp& b) { return b.baked && (plainLamps || !IsPlainType(b.type)); };
    for (uint64_t lot : now.settledLots) {
        // a lot that streamed in after that bake was never in it: the game does not rebuild for streaming, nor does Apex
        if (!std::binary_search(baked.lots.begin(), baked.lots.end(), lot)) continue;
        const auto [a0, a1] = LotLamps(baked, lot);
        const auto [b0, b1] = LotLamps(now, lot);
        const int before = d.added + d.removed + d.switchedOn + d.switchedOff + d.light;
        std::vector<char> used(static_cast<size_t>(b1 - b0), 0);
        for (auto a = a0; a != a1; ++a) {
            // the same lamp: same type, within 5 cm (matched by place, not by pointer: a lot that streamed out and back in
            // has new light objects for the same lamps)
            auto m = b1;
            for (auto b = b0; b != b1; ++b)
                if (!used[static_cast<size_t>(b - b0)] && b->type == a->type && !MovedApart(a->pos, b->pos)) {
                    m = b;
                    break;
                }
            if (m == b1) {
                if (inBake(*a)) d.removed++; // removed, or moved away
                continue;
            }
            used[static_cast<size_t>(m - b0)] = 1;
            const bool ia = inBake(*a), ib = inBake(*m);
            if (ia != ib) {
                if (m->animated) d.animated++;
                else if (ib) d.switchedOn++;
                else d.switchedOff++;
            } else if (ia && LightDiffers(a->light, m->light)) {
                if (m->animated) d.animated++;
                else d.light++;
            }
        }
        for (auto b = b0; b != b1; ++b)
            if (!used[static_cast<size_t>(b - b0)] && inBake(*b)) d.added++; // placed, or moved here
        if (d.added + d.removed + d.switchedOn + d.switchedOff + d.light != before) d.lots++;
    }
    return d;
}

std::string BakeDiff::Text() const {
    if (!Any()) return animated ? std::format("no change (animated lamps ignored: {})", animated) : std::string("no change");
    std::string t = std::format("{} lots: {} added, {} removed, {} switched on, {} switched off, {} relit", lots, added, removed, switchedOn, switchedOff, light);
    if (animated) t += std::format(" (animated lamps ignored: {})", animated);
    return t;
}

std::string RoofStatus() {
    return std::format("roofs: {} | lamps on: {} | draws fixed: {} | with snow: {}", g_roofFix.load() ? (g_roofPs ? "fixed" : "waiting") : "off",
                       g_lampCount, g_roofDrawn.load(), g_roofSnowDrawn.load());
}

std::string Status() {
    return std::format("{} | terrain chunks seen: {} | lot light fixed: {} draws (snow: {}, roads: {}, floors: {}, outdoor floors (summer): {}, snow on floors: {}, fences/stairs: {}, snow on objects: {}, snow with relief: {}, outdoor objects: {}) | without terrain texture: {}", g_status,
        g_chunks.size(), g_lotDrawn.load(), g_snowDrawn.load(), g_roadDrawn.load(), g_floorDrawn.load(), g_floorAtlasDrawn.load(), g_snowFloorDrawn.load(), g_fenceDrawn.load(), g_snowCoverDrawn.load(), g_snowReliefDrawn.load(), g_objLampDrawn.load(), g_lotMissing.load());
}

std::string DescribeDraw() {
    if (g_inOwnCall) return g_objDrawInfo.empty() ? std::string("mod draw (another fix)") : g_objDrawInfo;
    // without the hooks the remembered shaders are stale (possibly released)
    if (!g_hooksRegistered || g_hookFailed || g_stateUnknown) return "Night Lighting has no active hooks";
    static const char* kVs[] = {"other", "roof", "lake", "snowy lot", "road", "floor", "foliage", "fence/stairs (instanced)", "snow on object", "snow with relief",
                                "object with rig", "snowy floor"};
    static const char* kPs[] = {"unknown", "other", "world candidate", "lot light", "object rig", "roof", "lake", "snowy lot", "snowy roof",
                                "outside wall", "outdoor floor"};
    const auto itv = g_vsCache.find(g_curVs);
    const int vc = itv == g_vsCache.end() ? -1 : itv->second;
    const int pc = static_cast<int>(Classify(g_curPs));
    const int rig = RigTracker::CurrentMode();
    std::string s = std::format("game (the mod did not replace this draw) | VS {} | PS {} | rig mode {}", vc < 0 || vc > 11 ? "not classified" : kVs[vc], pc >= 0 && pc < 11 ? kPs[pc] : "?",
                                rig);
    if (vc == 10) {
        const FoliageVs& o = g_objectVs[g_curVs];
        s += std::format(" | object: world c{}, vertex lights c{}", o.worldK, o.vertexLight);
        const auto it = g_objLampPs.find(g_curPs);
        s += it == g_objLampPs.end() ? " | PS not tested yet" : it->second.ps ? " | PS accepted" : " | PS REFUSED (outside the pattern)";
        if (!g_objPixel.load(std::memory_order_relaxed)) s += " | object option off";
        if (rig != 1 && rig != 2) s += " | indoor rig (the game uses the lot light map; the mod leaves it)";
        float c[4];
        if (!LightmapSmooth::Atlas(c, false)) s += " | no ground light atlas";
    }
    return s;
}

std::string ObjectStatus() {
    return std::format("moon shadow on objects: {} | draws fixed: {} | foliage (wrap light): {} | winter foliage without shadow: {}",
                       g_objectFix.load() ? (g_objectPs ? "fixed" : "waiting") : "off", g_objectDrawn.load(), g_foliageDrawn.load(), g_leafDrawn.load());
}

void Shutdown(bool keepChunkMaps) {
    g_keepChunks = keepChunkMaps;
    g_objectFix = false;
    g_wallGain = 1.0f;
    g_roofFix = false;
    g_waterFix = false;
    g_objPixel = false;
    g_fenceFix = false;
    SetEnabled(false); // nothing left on: UpdateHooks unregisters the D3D hooks before the shaders are released
    if (g_replacementPs) {
        g_replacementPs->Release();
        g_replacementPs = nullptr;
    }
    if (g_snowPs) {
        g_snowPs->Release();
        g_snowPs = nullptr;
    }
    g_snowTried = false;
    for (auto* cache : {&g_roadPs, &g_floorPs, &g_snowFloorPs, &g_snowFloorPs0, &g_leafPs, &g_fencePs, &g_snowCoverPs, &g_snowReliefPs, &g_objLampPs}) {
        for (auto& [k, p] : *cache)
            if (p.ps) p.ps->Release();
        cache->clear();
    }
    for (auto& [k, f] : g_foliageVs)
        if (f.vs) f.vs->Release();
    g_foliageVs.clear();
    for (auto& [k, f] : g_objectVs)
        if (f.vs) f.vs->Release();
    g_objectVs.clear();
    g_snowFloorTc.clear();
    g_vsCache.clear();
    g_roadMapConst.clear();
    if (g_roofSnowPs) {
        g_roofSnowPs->Release();
        g_roofSnowPs = nullptr;
    }
    g_roofSnowTried = false;
    if (g_objectPs) {
        g_objectPs->Release();
        g_objectPs = nullptr;
    }
    g_objectCompileTried = false;
    g_objectFix = false;
    if (g_roofPs) {
        g_roofPs->Release();
        g_roofPs = nullptr;
    }
    if (g_waterPs) {
        g_waterPs->Release();
        g_waterPs = nullptr;
    }
    g_waterCompileTried = false;
    g_waterFix = false;
    g_roofCompileTried = false;
    g_roofFix = false;
    g_vsCache.clear();
    g_roadMapConst.clear();
    g_compileTried = false;
    g_classCache.clear();
    for (auto*& m : g_magenta)
        if (m) {
            m->Release();
            m = nullptr;
        }
    g_magentaTried = false;
    g_psIs3.clear();
    g_census.clear();
    g_falseColor = false;
    for (auto& [k, fv] : g_floorVs)
        if (fv.vs) fv.vs->Release();
    g_floorVs.clear();
    for (auto& [tc, cache] : g_floorAtlasPs)
        for (auto& [k, p] : cache)
            if (p.ps) p.ps->Release();
    g_floorAtlasPs.clear();
    g_wallConst.clear();
    for (IUnknown* p : g_pinned) p->Release();
    g_pinned.clear();
    g_curPs = nullptr;
    g_curVs = nullptr;
    g_stateUnknown = true;
    g_keepChunks = false;
}

} // namespace LotLightBridge
