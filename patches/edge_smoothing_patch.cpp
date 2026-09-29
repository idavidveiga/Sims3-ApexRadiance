// Edge Smoothing (FXAA)
// Post-process anti-aliasing of the 3D scene, applied right after the game finishes the scene and before it draws
// any UI (the same point as Depth Blur), so pie menus, tooltips and panels stay sharp.
//
// Cost, per frame:
//  - one StretchRect of the backbuffer into a texture of the same size (a plain copy, no filtering);
//  - one full-screen pass of FXAA (the "quality" algorithm of FXAA 3.11: edge detection on luma, search along the
//    edge, sub-pixel blend). Pixels with no local contrast leave after 5 texture reads (dynamic branch), which is most
//    of the screen; the edge search length is the quality setting.
//  - Triggered by PostScene (before Depth Blur).
//  - Only the states the pass touches are saved and restored (no state block). The render target is not changed.
// Works with the game's Edge Smoothing off (with it on, the game's own multisampling already smooths the edges).

#include "patch_base.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "post_scene.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include "build_flavor.h"
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"
#include "third_party/smaa/smaa_hlsl.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr const char* kHookName = "EdgeSmoothing";
constexpr int kRetryFrames = 120;

// FXAA "quality" (after FXAA 3.11 by Timothy Lottes, NVIDIA), written for ps_3_0 with tex2Dlod.
// STEPS and the step sizes come from the quality level (macros).
const char* kShaderSource = R"HLSL(
sampler2D sColor : register(s0);
float4 cRcp    : register(c0); // xy = 1 / screen size
float4 cParams : register(c1); // x = sub-pixel amount, y = edge threshold, z = edge threshold minimum, w = debug view

static const float kStep[STEPS] = { STEP_SIZES };

float Luma(float2 uv) { return dot(tex2Dlod(sColor, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114)); }

float4 FxaaPS(float2 pos : TEXCOORD0) : COLOR0
{
    const float2 rcp = cRcp.xy;
    float3 rgbM = tex2Dlod(sColor, float4(pos, 0, 0)).rgb;
    float lumaM = dot(rgbM, float3(0.299, 0.587, 0.114));
    float lumaS = Luma(pos + float2(0, rcp.y));
    float lumaE = Luma(pos + float2(rcp.x, 0));
    float lumaN = Luma(pos - float2(0, rcp.y));
    float lumaW = Luma(pos - float2(rcp.x, 0));
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    [branch] if (range < max(cParams.z, rangeMax * cParams.y))
        return float4(rgbM, 1);

    float lumaNW = Luma(pos - rcp);
    float lumaSE = Luma(pos + rcp);
    float lumaNE = Luma(pos + float2(rcp.x, -rcp.y));
    float lumaSW = Luma(pos + float2(-rcp.x, rcp.y));
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNESE);
    float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    // sub-pixel aliasing amount (thin features)
    float subpixB = ((lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE) * (1.0 / 12.0) - lumaM;
    float subpixC = saturate(abs(subpixB) / range);
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    float subpixH = subpixF * subpixF * cParams.x;

    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float lumaNN = (pairN ? lumaN : lumaS) + lumaM;

    // search both ways along the edge for its ends
    float2 posB = pos;
    float2 offNP = horzSpan ? float2(rcp.x, 0) : float2(0, rcp.y);
    if (horzSpan) posB.y += lengthSign * 0.5; else posB.x += lengthSign * 0.5;
    float2 posN = posB - offNP * kStep[0];
    float2 posP = posB + offNP * kStep[0];
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float lumaEndN = Luma(posN) - lumaNN * 0.5;
    float lumaEndP = Luma(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kStep[1];
    if (!doneP) posP += offNP * kStep[1];
    [unroll] for (int i = 2; i < STEPS; i++)
    {
        [branch] if (!doneN || !doneP)
        {
            if (!doneN) lumaEndN = Luma(posN) - lumaNN * 0.5;
            if (!doneP) lumaEndP = Luma(posP) - lumaNN * 0.5;
            doneN = doneN || abs(lumaEndN) >= gradientScaled;
            doneP = doneP || abs(lumaEndP) >= gradientScaled;
            if (!doneN) posN -= offNP * kStep[i];
            if (!doneP) posP += offNP * kStep[i];
        }
    }
    float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y;
    float dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;
    bool directionN = dstN < dstP;
    bool goodSpan = ((directionN ? lumaEndN : lumaEndP) < 0.0) != (lumaMM < 0.0);
    float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;
    float offset = max(pixelOffset, subpixH);
    if (horzSpan) pos.y += offset * lengthSign; else pos.x += offset * lengthSign;
    float3 rgb = tex2Dlod(sColor, float4(pos, 0, 0)).rgb;
    if (cParams.w > 0.5) rgb = lerp(rgb, float3(1, 0, 0), 0.6); // debug: the pixels FXAA touched
    return float4(rgb, 1);
}
)HLSL";

// Edge search: step sizes per quality level (FXAA 3.11 presets 12, 20-ish and 29).
struct QualityLevel {
    const char* steps;
    const char* sizes;
};
constexpr QualityLevel kQualities[3] = {
    {"5", "1.0, 1.5, 2.0, 4.0, 12.0"},
    {"8", "1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0"},
    {"12", "1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0"},
};

// ---- SMAA 1x (Jimenez, Echevarria, Masia, Navarro, Gutierrez 2012; reference code, MIT license, in third_party/smaa) ----
// Three passes: luma edge detection -> blending weights (searches along each edge and reads the precomputed area of the
// pattern it found, including diagonals and corners) -> neighbourhood blending. The reference SMAA.hlsl is embedded
// unchanged; only the wrapper below is ours. It computes in the pixel shader the offsets the reference computes in its
// vertex shaders (the effects here draw pre-transformed quads with no vertex shader).
const char* kSmaaPrefix = R"HLSL(
float4 cMetrics : register(c0); // 1/w, 1/h, w, h
float4 cParams  : register(c1); // x = threshold, w = debug view
#define SMAA_RT_METRICS cMetrics
#define SMAA_HLSL_3
#define SMAA_THRESHOLD cParams.x
)HLSL";
const char* kSmaaSuffix = R"HLSL(
sampler2D colorTex  : register(s0); // copy of the scene, linear
sampler2D edgesTex  : register(s1); // linear
sampler2D areaTex   : register(s2); // A8L8, linear
sampler2D searchTex : register(s3); // L8, point
sampler2D blendTex  : register(s4); // linear

float4 SmaaEdgePS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
    return float4(SMAALumaEdgeDetectionPS(uv, offset, colorTex), 0, 0);
}
float4 SmaaWeightPS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(uv, pixcoord, offset, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}
float4 SmaaBlendPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    float4 c = SMAANeighborhoodBlendingPS(uv, offset, colorTex, blendTex);
    if (cParams.w > 0.5)
    {
        float4 a = float4(tex2Dlod(blendTex, float4(offset.xy, 0, 0)).a, tex2Dlod(blendTex, float4(offset.zw, 0, 0)).g, tex2Dlod(blendTex, float4(uv, 0, 0)).xz);
        if (dot(a, 1.0) > 1e-5) c.rgb = lerp(c.rgb, float3(1, 0, 0), 0.6); // debug: the pixels SMAA blended
    }
    return float4(c.rgb, 1);
}
)HLSL";

// the reference presets (SMAA.hlsl, "SMAA Presets"), with the threshold as a shader constant
struct SmaaPreset {
    float threshold;
    const char* steps;
    const char* stepsDiag; // nullptr = diagonal and corner detection off
};
constexpr SmaaPreset kSmaaPresets[4] = {{0.15f, "4", nullptr}, {0.1f, "8", nullptr}, {0.1f, "16", "8"}, {0.05f, "32", "16"}};

// ---- every variant compiled at start-up on a background thread (framework/shader_cache.h) ----
// The render thread only creates the shader objects from the kept bytecode (first use, and after ReleaseShaders).
// Priority 0 = the default FXAA quality (Balanced) and SMAA preset (High).
ShaderCache::Id AddFxaa(int q, const char* tag) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kShaderSource;
    d.sourceName = "edge_smoothing.hlsl";
    d.entry = "FxaaPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.macros = {{"STEPS", kQualities[q].steps}, {"STEP_SIZES", kQualities[q].sizes}};
    d.priority = q == 1 ? 0 : 1;
    return ShaderCache::Add(std::move(d));
}
ShaderCache::Id AddSmaa(int q, int pass, const char* tag) {
    static const char* const kEntries[3] = {"SmaaEdgePS", "SmaaWeightPS", "SmaaBlendPS"};
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = std::string(kSmaaPrefix) + reinterpret_cast<const char*>(kSmaaHlsl) + kSmaaSuffix;
    d.sourceName = "SMAA.hlsl";
    d.entry = kEntries[pass];
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.macros.emplace_back("SMAA_MAX_SEARCH_STEPS", kSmaaPresets[q].steps);
    if (kSmaaPresets[q].stepsDiag) {
        d.macros.emplace_back("SMAA_MAX_SEARCH_STEPS_DIAG", kSmaaPresets[q].stepsDiag);
        d.macros.emplace_back("SMAA_CORNER_ROUNDING", "25");
    } else {
        d.macros.emplace_back("SMAA_DISABLE_DIAG_DETECTION", "1");
        d.macros.emplace_back("SMAA_DISABLE_CORNER_DETECTION", "1");
    }
    d.priority = q == 2 ? 0 : 1;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kFxaaPsId[3] = {AddFxaa(0, "EdgeSmoothing FXAA (Fast)"), AddFxaa(1, "EdgeSmoothing FXAA (Balanced)"), AddFxaa(2, "EdgeSmoothing FXAA (High)")};
const ShaderCache::Id kSmaaPsId[4][3] = {
    {AddSmaa(0, 0, "EdgeSmoothing SMAA edges (Low)"), AddSmaa(0, 1, "EdgeSmoothing SMAA weights (Low)"), AddSmaa(0, 2, "EdgeSmoothing SMAA blend (Low)")},
    {AddSmaa(1, 0, "EdgeSmoothing SMAA edges (Medium)"), AddSmaa(1, 1, "EdgeSmoothing SMAA weights (Medium)"), AddSmaa(1, 2, "EdgeSmoothing SMAA blend (Medium)")},
    {AddSmaa(2, 0, "EdgeSmoothing SMAA edges (High)"), AddSmaa(2, 1, "EdgeSmoothing SMAA weights (High)"), AddSmaa(2, 2, "EdgeSmoothing SMAA blend (High)")},
    {AddSmaa(3, 0, "EdgeSmoothing SMAA edges (Ultra)"), AddSmaa(3, 1, "EdgeSmoothing SMAA weights (Ultra)"), AddSmaa(3, 2, "EdgeSmoothing SMAA blend (Ultra)")},
};

struct Params {
    int method = 1;           // 0 FXAA, 1 SMAA
    int quality = 1;          // FXAA: 0 fast, 1 balanced, 2 high
    int smaaQuality = 2;      // SMAA: 0 low, 1 medium, 2 high, 3 ultra
    float subpix = 0.5f;      // sub-pixel smoothing (thin lines, texture detail): 0 = off, 1 = soft
    float sensitivity = 0.125f; // edge threshold: lower = more edges smoothed
    bool debugView = false;
};

struct AaState {
    bool active = false;
    bool ready = false;
    int retryCountdown = 0;
    unsigned framesSmoothed = 0;
    UINT width = 0, height = 0;
    IDirect3DTexture9* copyTex = nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    IDirect3DPixelShader9* ps[3] = {};
    bool compileTried[3] = {};
    // SMAA
    IDirect3DTexture9 *edgesTex = nullptr, *blendTex = nullptr, *areaTex = nullptr, *searchTex = nullptr;
    IDirect3DSurface9 *edgesSurf = nullptr, *blendSurf = nullptr;
    IDirect3DPixelShader9* smaaPs[4][3] = {};
    bool smaaTried[4] = {};
    // GPU cost (timestamp queries, read a few frames later)
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    int qNext = 0, qMethod = -1;
    float gpuMs = -1.0f;
    bool gameAaOn = false; // the game's own multisampled Edge Smoothing is on: paused (menu warning)
    std::string status = "Waiting for the game...";
    Params p;
};

AaState g;

template <typename T> void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void ReleaseResources() {
    g.ready = false;
    SafeRelease(g.copySurf);
    SafeRelease(g.copyTex);
    SafeRelease(g.edgesSurf);
    SafeRelease(g.blendSurf);
    SafeRelease(g.edgesTex);
    SafeRelease(g.blendTex);
    SafeRelease(g.areaTex);
    SafeRelease(g.searchTex);
    for (int i = 0; i < AaState::kQ; i++) {
        SafeRelease(g.qDisjoint[i]);
        SafeRelease(g.qBegin[i]);
        SafeRelease(g.qEnd[i]);
        SafeRelease(g.qFreq[i]);
        g.qIssued[i] = false;
    }
}

void ReleaseShaders() {
    for (int i = 0; i < 3; i++) {
        SafeRelease(g.ps[i]);
        g.compileTried[i] = false;
    }
    for (int q = 0; q < 4; q++) {
        for (auto& ps : g.smaaPs[q]) SafeRelease(ps);
        g.smaaTried[q] = false;
    }
}

// The three SMAA passes of one preset (created on first use from the precompiled bytecode)
bool SmaaShaders(IDirect3DDevice9* dev, int q) {
    q = std::clamp(q, 0, 3);
    if (g.smaaTried[q]) return g.smaaPs[q][0] && g.smaaPs[q][1] && g.smaaPs[q][2];
    g.smaaTried[q] = true;
    const char* entries[3] = {"SmaaEdgePS", "SmaaWeightPS", "SmaaBlendPS"};
    for (int i = 0; i < 3; i++) {
        std::string msg;
        switch (ShaderCache::CreatePixelShader(dev, kSmaaPsId[q][i], &g.smaaPs[q][i], &msg)) {
        case ShaderCache::Result::Ok:
            break;
        case ShaderCache::Result::CompileFailed:
            LOG_ERROR(std::format("[EdgeSmoothing] SMAA {} (preset {}) failed to compile: {}", entries[i], q, msg));
            g.status = "ERROR: SMAA did not compile (see ApexRadiance_LOG.txt)";
            break;
        case ShaderCache::Result::CreateFailed:
            LOG_ERROR(std::format("[EdgeSmoothing] CreatePixelShader({}) failed", entries[i]));
            break;
        }
    }
    return g.smaaPs[q][0] && g.smaaPs[q][1] && g.smaaPs[q][2];
}

// The two precomputed SMAA textures, from the reference headers: areaTex R8G8 -> A8L8 (the shader reads .ra, the
// reference's DX9 layout), searchTex R8 -> L8
bool CreateSmaaLookups(IDirect3DDevice9* dev) {
    auto upload = [&](IDirect3DTexture9** tex, UINT w, UINT h, D3DFORMAT fmt, const unsigned char* bytes, UINT pitch) {
        if (FAILED(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, tex, nullptr)) || !*tex) return false;
        D3DLOCKED_RECT lr{};
        if (FAILED((*tex)->LockRect(0, &lr, nullptr, 0))) return false;
        for (UINT y = 0; y < h; y++) std::memcpy(static_cast<unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, bytes + static_cast<size_t>(y) * pitch, pitch);
        (*tex)->UnlockRect(0);
        return true;
    };
    return upload(&g.areaTex, AREATEX_WIDTH, AREATEX_HEIGHT, D3DFMT_A8L8, areaTexBytes, AREATEX_PITCH) &&
           upload(&g.searchTex, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, D3DFMT_L8, searchTexBytes, SEARCHTEX_PITCH);
}

IDirect3DPixelShader9* ShaderFor(IDirect3DDevice9* dev, int q) {
    q = q < 0 ? 0 : q > 2 ? 2 : q;
    if (g.ps[q] || g.compileTried[q]) return g.ps[q];
    g.compileTried[q] = true;
    std::string msg;
    switch (ShaderCache::CreatePixelShader(dev, kFxaaPsId[q], &g.ps[q], &msg)) { // precompiled at start-up (shader_cache.h)
    case ShaderCache::Result::Ok:
        break;
    case ShaderCache::Result::CompileFailed:
        LOG_ERROR(std::format("[EdgeSmoothing] Shader (quality {}) failed to compile: {}", q, msg));
        g.status = "ERROR: the shader did not compile (see ApexRadiance_LOG.txt)";
        break;
    case ShaderCache::Result::CreateFailed:
        LOG_ERROR("[EdgeSmoothing] CreatePixelShader failed");
        break;
    }
    return g.ps[q];
}

bool InitResources(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();
    g.gameAaOn = bd.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (g.gameAaOn) {
        g.status = "The game's Edge Smoothing is on: turn it off in Options > Graphics to use this one";
        return false;
    }
    g.width = bd.Width;
    g.height = bd.Height;
    if (FAILED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g.copyTex, nullptr)) || !g.copyTex ||
        FAILED(g.copyTex->GetSurfaceLevel(0, &g.copySurf)) || !g.copySurf) {
        ReleaseResources();
        g.status = "ERROR: not enough video memory for the screen copy";
        return false;
    }
    auto make = [&](IDirect3DTexture9** tex, IDirect3DSurface9** surf) {
        return SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr)) && *tex &&
               SUCCEEDED((*tex)->GetSurfaceLevel(0, surf)) && *surf;
    };
    if (!make(&g.edgesTex, &g.edgesSurf) || !make(&g.blendTex, &g.blendSurf) || !CreateSmaaLookups(dev)) {
        ReleaseResources();
        g.status = "ERROR: not enough video memory for SMAA";
        return false;
    }
    for (int i = 0; i < AaState::kQ; i++) {
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g.qDisjoint[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qBegin[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qEnd[i]);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g.qFreq[i]);
    }
    g.ready = true;
    g.status = "Active";
    LOG_INFO(std::format("[EdgeSmoothing] Resources ready ({}x{})", g.width, g.height));
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

// ---- minimal state save/restore (only what the pass touches) ----
constexpr D3DRENDERSTATETYPE kRenderStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                                D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
                                                D3DRS_COLORWRITEENABLE};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
                                                  D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
constexpr int kRS = static_cast<int>(sizeof(kRenderStates) / sizeof(kRenderStates[0]));
constexpr int kSS = static_cast<int>(sizeof(kSamplerStates) / sizeof(kSamplerStates[0]));
constexpr UINT kPSConsts = 2;

void RunFxaa(IDirect3DDevice9* dev) {
    IDirect3DPixelShader9* ps = ShaderFor(dev, g.p.quality);
    if (!ps) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (FAILED(dev->StretchRect(bb, nullptr, g.copySurf, nullptr, D3DTEXF_NONE))) {
        bb->Release();
        return;
    }
    // save
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD rs[kRS], ss[kSS];
    float oldConst[kPSConsts * 4];
    D3DVIEWPORT9 oldVp{};
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    dev->GetTexture(0, &oldTex);
    for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
    for (int i = 0; i < kSS; i++) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
    dev->GetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->GetViewport(&oldVp);

    // pass: copy -> backbuffer (the render target is already the backbuffer)
    const D3DVIEWPORT9 vp{0, 0, g.width, g.height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetTexture(0, g.copyTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); // FXAA reads between texels on purpose
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    const float c[kPSConsts][4] = {{1.0f / static_cast<float>(g.width), 1.0f / static_cast<float>(g.height), 0, 0},
                                   {g.p.subpix, g.p.sensitivity, g.p.sensitivity / 3.0f, g.p.debugView ? 1.0f : 0.0f}};
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);
    dev->SetPixelShader(ps);
    const float x1 = static_cast<float>(g.width) - 0.5f, y1 = static_cast<float>(g.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));

    // restore
    dev->SetTexture(0, oldTex);
    for (int i = 0; i < kSS; i++) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldTex);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    bb->Release();
    g.framesSmoothed++;
}

void DrawQuad(IDirect3DDevice9* dev) {
    const float x1 = static_cast<float>(g.width) - 0.5f, y1 = static_cast<float>(g.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
}

// SMAA's three passes. Saves and restores what they touch (render target, samplers 0..4, shaders, constants c0..c1).
void RunSmaa(IDirect3DDevice9* dev) {
    const int q = std::clamp(g.p.smaaQuality, 0, 3);
    if (!SmaaShaders(dev, q)) return;
    IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    if (FAILED(dev->StretchRect(bb, nullptr, g.copySurf, nullptr, D3DTEXF_NONE))) {
        bb->Release();
        return;
    }
    constexpr DWORD kSamplers = 5;
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex[kSamplers] = {};
    DWORD rs[kRS], ss[kSamplers][kSS];
    float oldConst[kPSConsts * 4];
    D3DVIEWPORT9 oldVp{};
    dev->GetRenderTarget(0, &oldRt);
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->GetTexture(s, &oldTex[s]);
        for (int i = 0; i < kSS; i++) dev->GetSamplerState(s, kSamplerStates[i], &ss[s][i]);
    }
    for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
    dev->GetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->GetViewport(&oldVp);

    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (DWORD s = 0; s < kSamplers; s++) { // the reference: linear and clamp everywhere (the search texture read at texel centres)
        const DWORD f = s == 3 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }
    const float W = static_cast<float>(g.width), H = static_cast<float>(g.height);
    const float c[kPSConsts][4] = {{1.0f / W, 1.0f / H, W, H}, {kSmaaPresets[q].threshold, 0, 0, g.p.debugView ? 1.0f : 0.0f}};
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);

    // 1. edges (the targets are cleared every frame, alpha too: the passes discard where there is nothing to do)
    dev->SetRenderTarget(0, g.edgesSurf);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetTexture(0, g.copyTex);
    dev->SetPixelShader(g.smaaPs[q][0]);
    DrawQuad(dev);
    // 2. blending weights
    dev->SetRenderTarget(0, g.blendSurf);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, g.edgesTex);
    dev->SetTexture(2, g.areaTex);
    dev->SetTexture(3, g.searchTex);
    dev->SetPixelShader(g.smaaPs[q][1]);
    DrawQuad(dev);
    // 3. neighbourhood blending into the backbuffer (colour only: its alpha stays the game's)
    dev->SetRenderTarget(0, bb);
    dev->SetTexture(1, nullptr);
    dev->SetTexture(0, g.copyTex);
    dev->SetTexture(4, g.blendTex);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetPixelShader(g.smaaPs[q][2]);
    DrawQuad(dev);

    // restore
    dev->SetRenderTarget(0, oldRt); // resets the viewport, restored below
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->SetTexture(s, oldTex[s]);
        for (int i = 0; i < kSS; i++) dev->SetSamplerState(s, kSamplerStates[i], ss[s][i]);
        SafeRelease(oldTex[s]);
    }
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(0, oldConst, kPSConsts);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldRt);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    bb->Release();
    g.framesSmoothed++;
}

// GPU time of the effect, from timestamp queries of an earlier frame (never waits); restarts when the method changes
void ReadTimings() {
    for (int i = 0; i < AaState::kQ; i++) {
        if (!g.qIssued[i]) continue;
        BOOL disjoint = TRUE;
        UINT64 t0 = 0, t1 = 0, freq = 0;
        if (g.qDisjoint[i]->GetData(&disjoint, sizeof disjoint, 0) != S_OK || g.qBegin[i]->GetData(&t0, sizeof t0, 0) != S_OK ||
            g.qEnd[i]->GetData(&t1, sizeof t1, 0) != S_OK || g.qFreq[i]->GetData(&freq, sizeof freq, 0) != S_OK)
            continue;
        g.qIssued[i] = false;
        if (!disjoint && freq && t1 > t0) {
            const float ms = static_cast<float>(double(t1 - t0) * 1000.0 / double(freq));
            g.gpuMs = g.gpuMs < 0 ? ms : g.gpuMs * 0.9f + ms * 0.1f;
        }
    }
}

// PostScene effect (order kEdgeSmoothing): before Depth Blur and the UI
void FxaaEffect(IDirect3DDevice9* dev) {
    if (!g.ready) return;
    const int method = g.p.method == 1 ? 1 : 0;
    const int key = method * 10 + (method ? g.p.smaaQuality : g.p.quality);
    if (key != g.qMethod) {
        g.qMethod = key;
        g.gpuMs = -1.0f;
        for (bool& b : g.qIssued) b = false;
    }
    ReadTimings();
    const int qi = g.qNext;
    const bool timed = !g.qIssued[qi] && g.qDisjoint[qi] && g.qBegin[qi] && g.qEnd[qi] && g.qFreq[qi];
    if (timed) {
        g.qDisjoint[qi]->Issue(D3DISSUE_BEGIN);
        g.qBegin[qi]->Issue(D3DISSUE_END);
    }
    if (method == 1) RunSmaa(dev);
    else RunFxaa(dev);
    if (timed) {
        g.qEnd[qi]->Issue(D3DISSUE_END);
        g.qFreq[qi]->Issue(D3DISSUE_END);
        g.qDisjoint[qi]->Issue(D3DISSUE_END);
        g.qIssued[qi] = true;
        g.qNext = (qi + 1) % AaState::kQ;
    }
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    if (!g.active) return;
    if (!g.ready && --g.retryCountdown <= 0) {
        g.retryCountdown = kRetryFrames;
        InitResources(dev);
    }
}

void OnPreReset(IDirect3DDevice9*) {
    if (!g.active) return;
    ReleaseResources();
    g.status = "Recreating after a video change...";
}

void OnPostReset(IDirect3DDevice9*) {
    if (!g.active) return;
    g.retryCountdown = 0;
}

} // namespace

class EdgeSmoothingPatch : public ApexPatch {
  public:
    EdgeSmoothingPatch() : ApexPatch("EdgeSmoothing", nullptr) {
        RegisterEnumSetting(&g.p.method, "metodo", 1, "SMAA: smoother long edges and sharp textures (3 passes). FXAA: lighter, a little blurrier.",
                            {"FXAA", "SMAA"});
        RegisterEnumSetting(&g.p.smaaQuality, "qualidadeSmaa", 2,
                            "SMAA: the reference presets. High and Ultra also handle diagonals and corners; Ultra catches fainter edges (good at night).",
                            {"Low", "Medium", "High", "Ultra"});
        RegisterEnumSetting(&g.p.quality, "qualidade", 1,
                            "FXAA: how far each edge is followed. Higher = smoother long, nearly straight edges, costs a little more.",
                            {"Fast", "Balanced", "High"});
        RegisterFloatSetting(&g.p.subpix, "suavidade", SettingWidget::Slider, 0.5f, 0.0f, 1.0f,
                             "Also smooths thin lines and sub-pixel detail. Higher = smoother, but textures get slightly softer.");
        RegisterFloatSetting(&g.p.sensitivity, "sensibilidade", SettingWidget::Slider, 0.125f, 0.063f, 0.333f,
                             "Minimum contrast for an edge to be smoothed. Lower = catches more edges (also in dark night scenes).");
        RegisterBoolSetting(&g.p.debugView, "debugView", false, "Show the smoothed pixels in red");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        using namespace D3D9Hooks;
        RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnFrameBoundary(ctx.device);
            return HookAction::Continue;
        }, Priority::First);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Add(RenderCallbacks::postReset, OnPostReset);
        PostScene::Add(PostScene::kEdgeSmoothing, FxaaEffect);
        g.active = true;
        g.retryCountdown = 0;
        g.status = "Waiting for the game...";
        isEnabled = true;
        LOG_INFO("[EdgeSmoothing] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        PostScene::Remove(FxaaEffect);
        g.active = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Remove(RenderCallbacks::postReset, OnPostReset);
        ReleaseResources();
        ReleaseShaders();
        g.status = "Off";
        g.gameAaOn = false;
        isEnabled = false;
        LOG_INFO("[EdgeSmoothing] Uninstalled");
        return true;
    }

    // Settings are read live every frame
    void Update() override { pendingReinstall = false; }

    // Overview row and card header chip (the smoothing pass, timed with timestamp queries)
    float GpuCostMs() const override { return (isEnabled.load() && g.ready && g.gpuMs >= 0.0f) ? g.gpuMs : -1.0f; }

    // The card's controls (menu: System > Display, Anti-aliasing tab). Settings are read live every frame; the change notice only
    // keeps the base class informed and saves.
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        static const Params kDefaults{}; // the registered defaults (changed dots and per-row Reset)
        bool changed = false;
        if (g.gameAaOn) ApexUi::IconNote(IconId::TriangleAlert, "Paused while the game's own Edge Smoothing is on (Options \xE2\x80\xBA Graphics)", VioletTheme::kWarning);
        else if (g.status.rfind("ERROR: ", 0) == 0) ApexUi::IconNote(IconId::TriangleAlert, g.status.c_str() + 7, VioletTheme::kError);

        // Method: SMAA first (index 0 = method 1)
        static const char* const kMethods[] = {"SMAA (recommended)", "FXAA"};
        static const char* const kMethodTips[] = {"Clean, smooth edges while textures stay sharp", "Lighter on your graphics card, a little blurrier"};
        int method = g.p.method == 1 ? 0 : 1;
        if (ApexUi::SegmentedRow("Method", "SMAA looks cleanest, FXAA is the lightest", "##Method", &method, kMethods, 2, kMethodTips, nullptr, 0)) { // default: SMAA (index 0)
            g.p.method = method == 0 ? 1 : 0;
            changed = true;
        }
        if (g.p.method == 1) {
            static const char* const kSmaa[] = {"Low", "Medium", "High", "Ultra"};
            static const char* const kSmaaTips[] = {"Fastest; smooths the clearest edges", "A good balance", "Also smooths diagonals and corners",
                                                    "Catches faint edges too, great at night; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality##Smaa", "Higher smooths more edges and costs a bit more", "##SmaaQuality", &g.p.smaaQuality, kSmaa, 4, kSmaaTips, nullptr, kDefaults.smaaQuality);
        } else {
            static const char* const kFxaa[] = {"Fast", "Balanced", "High"};
            static const char* const kFxaaTips[] = {"Fastest", "A good balance", "Smoother long edges; costs a little more"};
            changed |= ApexUi::SegmentedRow("Quality##Fxaa", "Higher smooths more edges and costs a bit more", "##FxaaQuality", &g.p.quality, kFxaa, 3, kFxaaTips, nullptr, kDefaults.quality);
            // FXAA's own tuning (SMAA uses its reference presets), shown with FXAA only
            changed |= ApexUi::SliderPercent("Softness", &g.p.subpix, 0.0f, 1.0f, "Also smooths thin lines; higher softens textures a bit", kDefaults.subpix);
            // Shown as 0-100% (higher catches fainter edges); stored as the edge threshold (lower catches more)
            constexpr float kHi = 0.333f, kLo = 0.063f;
            float sensitivity = (kHi - g.p.sensitivity) / (kHi - kLo);
            if (ApexUi::SliderPercent("Sensitivity", &sensitivity, 0.0f, 1.0f, "Higher catches fainter edges, also at night", (kHi - kDefaults.sensitivity) / (kHi - kLo))) {
                g.p.sensitivity = kHi - sensitivity * (kHi - kLo);
                changed = true;
            }
        }
        if (ApexUi::IconTextButton("Reset Edge Smoothing##EdgeSmoothing", IconId::RotateCcw, "Back to SMAA, High quality")) {
            ApexUi::ReportChange("Edge Smoothing reset");
            g.p = Params{};
            changed = true;
        }
        if (changed) NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        ImGui::TextWrapped("Status: %s", g.status.c_str());
        if (g.ready && g.gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame", g.gpuMs);
        bool changed = ImGui::Checkbox("Show smoothed pixels in red", &g.p.debugView);
        ApexUi::Tooltip("Tints every pixel the smoothing changed red, to see which edges it catches");
        if (g.ready) ImGui::TextDisabled("Frames smoothed: %u", g.framesSmoothed);
        if (changed) NotifySettingChanged();
    }
};

APEX_REGISTER_FEATURE(EdgeSmoothingPatch, {.displayName = "Edge Smoothing (SMAA / FXAA)",
                                    .description = "Smooths the jagged edges of the world while menus and text stay sharp. Works with the game's own "
                                                   "Edge Smoothing turned off. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                    .category = "Graphics",
                                    .experimental = true,
                                    .supportedVersions = VERSION_ALL,
                                    .technicalDetails = {"Runs before the first ZENABLE=FALSE backbuffer draw after the scene (bloom composite / UI start), like Depth Blur.",
                                                         "SMAA 1x: the reference SMAA.hlsl (MIT, third_party/smaa), luma edges -> blending weights (area/search textures) -> neighbourhood blending; presets Low..Ultra.",
                                                         "FXAA: one full-screen FXAA 3.11-style pass (luma edge detection, edge search, sub-pixel blend).",
                                                         "One StretchRect copy of the backbuffer; saves/restores only the states it touches. GPU cost measured with timestamp queries.",
                                                         "Disabled while the game's multisampled Edge Smoothing is on."}})
