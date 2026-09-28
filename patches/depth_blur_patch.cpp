// Depth Blur
// Blurs distant scenery based on the scene depth buffer, applied right after the game finishes the 3D scene
// and before it draws any UI, so pie menus, tooltips and panels stay sharp.
//
// How it works (see ApexRadiance_FrameCapture.txt analysis):
//  - The game renders the 3D scene straight into the backbuffer using the auto depth-stencil, then the bloom
//    composite and the UI, which are the first backbuffer draws with ZENABLE = FALSE after the scene.
//  - The auto depth-stencil is swapped for an INTZ depth texture (same size, readable by shaders) through the
//    SetDepthStencilSurface / GetDepthStencilSurface detours; the game never sees the swap.
//  - Right before the first ZENABLE = FALSE backbuffer draw of each frame (PostScene, after edge smoothing):
//      1. StretchRect the backbuffer straight to half resolution (bilinear)
//      2. Prep pass: rgb = color, a = blur factor from linearized depth (computed once per pixel)
//      3. N separable Gaussian passes at half resolution, taps weighted by their own blur factor (no halos)
//      4. Composite: the blurred image is alpha-blended over the backbuffer, alpha = full-res blur factor
//  - Only the handful of states the passes touch are saved/restored (no full state block, which is CPU heavy).
// Requires the game's Edge Smoothing to be off (multisampled depth cannot be read in D3D9).

#include "patch_base.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
#include "post_scene.h"
#include "map_view.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include "build_flavor.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace {

constexpr const char* kHookName = "DepthBlur";
constexpr D3DFORMAT kFmtINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr int kRetryFrames = 120;
constexpr float kMapFadeSeconds = 0.3f;

const char* kShaderSource = R"HLSL(
sampler2D sColor : register(s0);
sampler2D sBlur  : register(s1);
sampler2D sDepth : register(s2);
float4 cParams : register(c0); // x = start, y = range, z = strength, w = far plane
float4 cTexel  : register(c1); // xy = 1 / size of the color source
float4 cDir    : register(c2); // xy = direction * tap spacing
float4 cFlags  : register(c3); // x = blur sky, y = debug view

float BlurFactor(float2 uv)
{
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float lin = d / (cParams.w - d * (cParams.w - 1.0));
    float f = saturate((lin - cParams.x) / max(cParams.y, 0.0001));
    if (d >= 0.99999) f = cFlags.x;
    return f * cParams.z;
}

// Half-res color in, rgb = color, a = blur factor of that pixel
float4 PrepPS(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(tex2Dlod(sColor, float4(uv, 0, 0)).rgb, BlurFactor(uv));
}

static const float W[7] = { 0.1963, 0.1745, 0.1216, 0.0662, 0.0280, 0.0092, 0.0024 };

float4 BlurPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 center = tex2Dlod(sColor, float4(uv, 0, 0));
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int i = -6; i <= 6; i++)
    {
        float4 s = tex2Dlod(sColor, float4(uv + cDir.xy * cTexel.xy * i, 0, 0));
        // Weight taps by their own blur factor so sharp foreground does not bleed into the blurred background
        float w = W[abs(i)] * (s.a + 0.02);
        acc += s.rgb * w;
        wsum += w;
    }
    return float4(acc / wsum, center.a);
}

// Drawn with alpha blending over the backbuffer: alpha = how blurred this pixel should be
float4 CompositePS(float2 uv : TEXCOORD0) : COLOR0
{
    float f = BlurFactor(uv);
    if (cFlags.y > 0.5) return float4(f, f, f, 1);
    return float4(tex2Dlod(sBlur, float4(uv, 0, 0)).rgb, f);
}
)HLSL";

struct Params {
    float start = 0.349f;
    float range = 0.20f;
    float strength = 1.0f;
    float spread = 1.5f;
    float farPlane = 1000.0f;
    bool blurSky = true;
    bool debugView = false;
    bool offInMapView = true; // no blur while the game's map view is open (everything is far away there)
    int quality = 2; // 0 Low, 1 Medium, 2 High, 3 Ultra
};

// Max tap spacing (half-res pixels) and max pass count per quality level
constexpr float kQualitySpacing[4] = {2.0f, 1.4f, 1.0f, 0.75f};
constexpr int kQualityMaxIterations[4] = {1, 3, 6, 12};

int BlurIterations(float spread, int quality) {
    const int q = (quality < 0) ? 0 : (quality > 3 ? 3 : quality);
    const float ratio = spread / kQualitySpacing[q];
    int n = static_cast<int>(std::ceil(ratio * ratio));
    if (n < 1) n = 1;
    if (n > kQualityMaxIterations[q]) n = kQualityMaxIterations[q];
    return n;
}

struct BlurState {
    bool active = false; // the depth swap is running (Depth Blur on, or requested by another effect)
    bool blurOn = false; // the Depth Blur patch itself is on
    int requests = 0;    // DepthShare::Request from other effects
    bool ready = false;
    bool inBlur = false;
    bool internalPass = false; // another patch's own extra draw (e.g. the lake lamp pass with the depth-stencil unbound)
    int retryCountdown = 0;
    unsigned framesBlurred = 0;
    int lastIterations = 0;
    float mapFade = 0.0f;         // 0 = normal view, 1 = map view (blur fully off), eased over kMapFadeSeconds
    LARGE_INTEGER lastFadeTick{}; // time of the previous fade step
    bool mapOpen = false;         // last map view state read from the game

    IDirect3DSurface9* curRT0 = nullptr;     // identity only
    IDirect3DSurface9* backBuffer = nullptr; // identity only
    UINT width = 0;
    UINT height = 0;

    IDirect3DSurface9* origDS = nullptr; // game's auto depth-stencil (reference held)
    IDirect3DTexture9* intzTex = nullptr;
    IDirect3DSurface9* intzSurf = nullptr;
    IDirect3DTexture9* halfATex = nullptr;
    IDirect3DSurface9* halfASurf = nullptr;
    IDirect3DTexture9* halfBTex = nullptr;
    IDirect3DSurface9* halfBSurf = nullptr;
    IDirect3DPixelShader9* psPrep = nullptr;
    IDirect3DPixelShader9* psBlur = nullptr;
    IDirect3DPixelShader9* psComposite = nullptr;

    bool gameAaOn = false; // the game's own multisampled Edge Smoothing is on: no readable depth (menu warning)
    std::string status = "Waiting for the game...";
    Params p;
};

BlurState g;

template <typename T> void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

IDirect3DSurface9* SubstituteDS(IDirect3DSurface9* requested) { return (g.ready && requested && requested == g.origDS) ? g.intzSurf : requested; }

IDirect3DSurface9* ReportDS(IDirect3DSurface9* actual) { return (g.ready && actual && actual == g.intzSurf) ? g.origDS : actual; }

void ReleaseResources(IDirect3DDevice9* dev) {
    ExtraHooks::SetDepthSubstitution(nullptr, nullptr);
    if (dev && g.intzSurf && g.origDS) {
        IDirect3DSurface9* cur = nullptr;
        if (SUCCEEDED(ExtraHooks::RawGetDepthStencilSurface(dev, &cur)) && cur) {
            if (cur == g.intzSurf) ExtraHooks::RawSetDepthStencilSurface(dev, g.origDS);
            cur->Release();
        }
    }
    g.ready = false;
    SafeRelease(g.intzSurf);
    SafeRelease(g.intzTex);
    SafeRelease(g.halfASurf);
    SafeRelease(g.halfATex);
    SafeRelease(g.halfBSurf);
    SafeRelease(g.halfBTex);
    SafeRelease(g.origDS);
}

void ReleaseShaders() {
    SafeRelease(g.psPrep);
    SafeRelease(g.psBlur);
    SafeRelease(g.psComposite);
}

IDirect3DPixelShader9* CompileShader(IDirect3DDevice9* dev, const char* entry) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "depth_blur.hlsl", nullptr, nullptr, entry, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "unknown error";
        LOG_ERROR(std::format("[DepthBlur] Shader {} failed to compile: {}", entry, msg));
        if (errors) errors->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    IDirect3DPixelShader9* ps = nullptr;
    if (FAILED(dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps))) {
        LOG_ERROR(std::format("[DepthBlur] CreatePixelShader({}) failed", entry));
        ps = nullptr;
    }
    code->Release();
    return ps;
}

bool CreateRT(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DTexture9** tex, IDirect3DSurface9** surf) {
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, tex, nullptr)) || !*tex) return false;
    return SUCCEEDED((*tex)->GetSurfaceLevel(0, surf)) && *surf;
}

bool InitResources(IDirect3DDevice9* dev) {
    if (!ExtraHooks::EnsureInstalled(dev)) {
        g.status = "ERROR: could not install the depth hooks";
        return false;
    }

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    bb->Release();

    g.gameAaOn = bd.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (g.gameAaOn) {
        g.status = "Edge Smoothing is on: turn it off in the game's Options > Graphics";
        return false;
    }

    IDirect3DSurface9* ds = nullptr;
    if (FAILED(ExtraHooks::RawGetDepthStencilSurface(dev, &ds)) || !ds) {
        g.status = "Waiting for the game (no depth buffer yet)";
        return false;
    }
    D3DSURFACE_DESC dd{};
    ds->GetDesc(&dd);
    if (dd.Width != bd.Width || dd.Height != bd.Height || dd.MultiSampleType != D3DMULTISAMPLE_NONE || (dd.Format != D3DFMT_D24S8 && dd.Format != D3DFMT_D24X8)) {
        ds->Release();
        g.status = "Waiting for the game (depth buffer is not the screen's)";
        return false;
    }

    g.origDS = ds; // keep the reference
    g.width = bd.Width;
    g.height = bd.Height;
    const UINT hw = (g.width + 1) / 2;
    const UINT hh = (g.height + 1) / 2;

    bool ok = SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_DEPTHSTENCIL, kFmtINTZ, D3DPOOL_DEFAULT, &g.intzTex, nullptr)) && g.intzTex &&
              SUCCEEDED(g.intzTex->GetSurfaceLevel(0, &g.intzSurf)) && g.intzSurf;
    if (!ok) {
        ReleaseResources(dev);
        g.status = "ERROR: the graphics card/driver does not support INTZ depth textures";
        return false;
    }
    // A8R8G8B8 on purpose: the alpha channel carries the per-pixel blur factor between passes
    if (!CreateRT(dev, hw, hh, D3DFMT_A8R8G8B8, &g.halfATex, &g.halfASurf) || !CreateRT(dev, hw, hh, D3DFMT_A8R8G8B8, &g.halfBTex, &g.halfBSurf)) {
        ReleaseResources(dev);
        g.status = "ERROR: not enough video memory for the blur textures";
        return false;
    }
    if (!g.psPrep) g.psPrep = CompileShader(dev, "PrepPS");
    if (!g.psBlur) g.psBlur = CompileShader(dev, "BlurPS");
    if (!g.psComposite) g.psComposite = CompileShader(dev, "CompositePS");
    if (!g.psPrep || !g.psBlur || !g.psComposite) {
        ReleaseResources(dev);
        g.status = "ERROR: the blur shaders did not compile (see ApexRadiance_LOG.txt)";
        return false;
    }

    IDirect3DSurface9* rt0 = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt0)) && rt0) {
        g.curRT0 = rt0;
        rt0->Release();
    }

    ExtraHooks::SetDepthSubstitution(SubstituteDS, ReportDS);
    g.ready = true;
    ExtraHooks::RawSetDepthStencilSurface(dev, g.intzSurf); // the auto depth-stencil is bound right now
    g.status = "Active";
    LOG_INFO(std::format("[DepthBlur] Resources ready ({}x{}, INTZ depth swapped in)", g.width, g.height));
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h) {
    const float x0 = -0.5f, y0 = -0.5f;
    const float x1 = static_cast<float>(w) - 0.5f, y1 = static_cast<float>(h) - 0.5f;
    const QuadVertex v[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
}

// ---- minimal state save/restore (only what the passes touch) ----

constexpr D3DRENDERSTATETYPE kRenderStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
    D3DRS_COLORWRITEENABLE};
constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
constexpr int kRS = static_cast<int>(sizeof(kRenderStates) / sizeof(kRenderStates[0]));
constexpr int kSS = static_cast<int>(sizeof(kSamplerStates) / sizeof(kSamplerStates[0]));
constexpr DWORD kSamplers = 3;
constexpr UINT kPSConsts = 4;

struct SavedState {
    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* stream0 = nullptr;
    UINT stream0Offset = 0;
    UINT stream0Stride = 0;
    IDirect3DBaseTexture9* tex[kSamplers] = {};
    DWORD rs[kRS] = {};
    DWORD ss[kSamplers][kSS] = {};
    float psConst[kPSConsts * 4] = {};
    D3DVIEWPORT9 viewport{};

    void Capture(IDirect3DDevice9* dev) {
        dev->GetRenderTarget(0, &rt0);
        ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
        dev->GetPixelShader(&ps);
        dev->GetVertexShader(&vs);
        dev->GetVertexDeclaration(&decl);
        dev->GetFVF(&fvf);
        dev->GetStreamSource(0, &stream0, &stream0Offset, &stream0Stride);
        for (DWORD s = 0; s < kSamplers; s++) {
            dev->GetTexture(s, &tex[s]);
            for (int i = 0; i < kSS; i++) dev->GetSamplerState(s, kSamplerStates[i], &ss[s][i]);
        }
        for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
        dev->GetPixelShaderConstantF(0, psConst, kPSConsts);
        dev->GetViewport(&viewport);
    }

    void Restore(IDirect3DDevice9* dev) {
        dev->SetRenderTarget(0, rt0); // resets the viewport, so it goes first
        ExtraHooks::RawSetDepthStencilSurface(dev, ds);
        for (DWORD s = 0; s < kSamplers; s++) {
            dev->SetTexture(s, tex[s]);
            for (int i = 0; i < kSS; i++) dev->SetSamplerState(s, kSamplerStates[i], ss[s][i]);
        }
        for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
        dev->SetPixelShaderConstantF(0, psConst, kPSConsts);
        dev->SetPixelShader(ps);
        dev->SetVertexShader(vs);
        if (decl) {
            dev->SetVertexDeclaration(decl);
        } else {
            dev->SetFVF(fvf);
        }
        // DrawPrimitiveUP clears stream 0, the game's next draw needs it back
        dev->SetStreamSource(0, stream0, stream0Offset, stream0Stride);
        dev->SetViewport(&viewport);
        Release();
    }

    void Release() {
        SafeRelease(rt0);
        SafeRelease(ds);
        SafeRelease(ps);
        SafeRelease(vs);
        SafeRelease(decl);
        SafeRelease(stream0);
        for (auto& t : tex) SafeRelease(t);
    }
};

void SetPassStates(IDirect3DDevice9* dev) {
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
    for (DWORD s = 0; s < kSamplers; s++) {
        const DWORD filter = (s == 2) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }
}

void RunBlur(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

    g.inBlur = true;
    SavedState saved;
    saved.Capture(dev);

    const UINT hw = (g.width + 1) / 2;
    const UINT hh = (g.height + 1) / 2;

    // 1. Backbuffer straight to half resolution (bilinear), no full-res copy
    dev->StretchRect(bb, nullptr, g.halfBSurf, nullptr, D3DTEXF_LINEAR);

    // Depth must not be bound while it is sampled
    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr);
    SetPassStates(dev);

    const float strength = g.p.strength * (1.0f - g.mapFade);
    const float c0[4] = {g.p.start, g.p.range, strength, g.p.farPlane};
    const float c1[4] = {1.0f / static_cast<float>(hw), 1.0f / static_cast<float>(hh), 0, 0};
    const float c3[4] = {g.p.blurSky ? 1.0f : 0.0f, g.p.debugView ? 1.0f : 0.0f, 0, 0};
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(3, c3, 1);
    dev->SetTexture(2, g.intzTex);

    // 2. Prep: halfB (color) -> halfA (color + blur factor in alpha)
    dev->SetPixelShader(g.psPrep);
    dev->SetRenderTarget(0, g.halfASurf);
    dev->SetTexture(0, g.halfBTex);
    DrawQuad(dev, hw, hh);

    // 3. Blur: repeat the separable pass with tight tap spacing instead of spreading taps apart (no "dots")
    //    n Gaussian passes of spacing s equal one pass of spacing s * sqrt(n)
    const int iterations = BlurIterations(g.p.spread, g.p.quality);
    const float spacing = g.p.spread / std::sqrt(static_cast<float>(iterations));
    const float cH[4] = {spacing, 0, 0, 0};
    const float cV[4] = {0, spacing, 0, 0};
    dev->SetPixelShader(g.psBlur);
    for (int it = 0; it < iterations; it++) {
        dev->SetRenderTarget(0, g.halfBSurf);
        dev->SetTexture(0, g.halfATex);
        dev->SetPixelShaderConstantF(2, cH, 1);
        DrawQuad(dev, hw, hh);

        dev->SetRenderTarget(0, g.halfASurf);
        dev->SetTexture(0, g.halfBTex);
        dev->SetPixelShaderConstantF(2, cV, 1);
        DrawQuad(dev, hw, hh);
    }
    g.lastIterations = iterations;

    // 4. Composite: blend the blurred image over the backbuffer, alpha = full-res blur factor (RGB only)
    dev->SetRenderTarget(0, bb);
    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, g.halfATex);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetPixelShader(g.psComposite);
    DrawQuad(dev, g.width, g.height);

    saved.Restore(dev);
    bb->Release();
    g.inBlur = false;
    g.framesBlurred++;
}

// PostScene effect (order kDepthBlur): after edge smoothing, before the UI
// Eases the map view fade toward the game's current state (once per frame, from the PostScene trigger)
void StepMapFade() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g.lastFadeTick.QuadPart ? static_cast<float>(now.QuadPart - g.lastFadeTick.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    g.lastFadeTick = now;
    if (dt > 0.1f) dt = 0.1f; // a hitch or a long pause should not skip the fade
    g.mapOpen = g.p.offInMapView && MapView::IsOpen();
    const float target = g.mapOpen ? 1.0f : 0.0f;
    const float step = dt / kMapFadeSeconds;
    g.mapFade = (g.mapFade < target) ? std::fmin(g.mapFade + step, target) : std::fmax(g.mapFade - step, target);
}

void BlurEffect(IDirect3DDevice9* dev) {
    if (!g.blurOn || !g.ready || g.inBlur || g.internalPass) return;
    StepMapFade();
    if (g.p.debugView) {
        RunBlur(dev);
        return;
    }
    if (g.p.strength * (1.0f - g.mapFade) <= 0.0f) return; // map view: no GPU work at all
    RunBlur(dev);
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    if (!g.active) return;
    if (!g.ready && --g.retryCountdown <= 0) {
        g.retryCountdown = kRetryFrames;
        InitResources(dev);
    }
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g.backBuffer = bb;
        bb->Release();
    }
}

void OnPreReset(IDirect3DDevice9* dev) {
    if (!g.active) return;
    ReleaseResources(dev);
    g.status = "Recreating after a video change...";
}

void OnPostReset(IDirect3DDevice9*) {
    if (!g.active) return;
    g.retryCountdown = 0;
    g.backBuffer = nullptr;
    g.curRT0 = nullptr;
}

// The depth swap runs while Depth Blur is on or another effect asked for the depth (DepthShare::Request).
void StartDepth() {
    if (g.active) return;
    D3D9Hooks::RegisterPresent(kHookName, [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::First);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    RenderCallbacks::Add(RenderCallbacks::postReset, OnPostReset);
    g.active = true;
    g.retryCountdown = 0;
    g.status = "Waiting for the game...";
}

void StopDepth() {
    if (!g.active) return;
    g.active = false;
    D3D9Hooks::UnregisterAll(kHookName);
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    RenderCallbacks::Remove(RenderCallbacks::postReset, OnPostReset);
    ReleaseResources(ApexD3D::Device());
    g.gameAaOn = false;
}

void UpdateDepth() {
    if (g.blurOn || g.requests > 0) StartDepth();
    else StopDepth();
}

} // namespace

namespace DepthShare {
IDirect3DTexture9* Texture() { return g.ready ? g.intzTex : nullptr; }
IDirect3DSurface9* Surface() { return g.ready ? g.intzSurf : nullptr; }
void SetInternalPass(bool on) { g.internalPass = on; }
bool InternalPass() { return g.internalPass; }
void Request(bool on) {
    g.requests = on ? g.requests + 1 : (g.requests > 0 ? g.requests - 1 : 0);
    UpdateDepth();
}
std::string Status() { return g.status; }
} // namespace DepthShare

class DepthBlurPatch : public ApexPatch {
  public:
    DepthBlurPatch() : ApexPatch("DepthBlur", nullptr) {
        RegisterFloatSetting(&g.p.start, "distancia", SettingWidget::Slider, 0.349f, 0.0f, 0.5f, "Where the blur starts. Higher = further from the camera.",
            {{"Near", 0.25f}, {"Medium", 0.349f}, {"Far", 0.45f}});
        RegisterFloatSetting(&g.p.range, "transicao", SettingWidget::Slider, 0.20f, 0.01f, 0.5f,
            "How far the blur takes to reach full strength. Lower = sharper transition.");
        RegisterFloatSetting(&g.p.strength, "forca", SettingWidget::Slider, 1.0f, 0.0f, 1.0f, "Amount of blur in the background. 0 = none, 1 = full.");
        RegisterFloatSetting(&g.p.spread, "tamanho", SettingWidget::Slider, 1.5f, 0.5f, 6.0f, "Blur radius. Higher = blurrier background.");
        RegisterEnumSetting(&g.p.quality, "qualidade", 2, "Blur quality. Higher = smoother large blur, costs more GPU.", {"Low", "Medium", "High", "Ultra"});
        RegisterFloatSetting(&g.p.farPlane, "farPlane", SettingWidget::InputBox, 1000.0f, 10.0f, 10000.0f, "Depth linearization (advanced)");
        RegisterBoolSetting(&g.p.blurSky, "blurSky", true, "Blur the sky");
        RegisterBoolSetting(&g.p.offInMapView, "offInMapView", true, "Turn the blur off while the map view is open");
        RegisterBoolSetting(&g.p.debugView, "debugView", false, "Show the mask (white = blurred, black = sharp)");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        g.blurOn = true;
        UpdateDepth();
        PostScene::Add(PostScene::kDepthBlur, BlurEffect);
        isEnabled = true;
        LOG_INFO("[DepthBlur] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        PostScene::Remove(BlurEffect);
        g.blurOn = false;
        UpdateDepth(); // keeps the depth swap while another effect still needs it
        ReleaseShaders();
        if (!g.active) g.status = "Off";
        isEnabled = false;
        LOG_INFO("[DepthBlur] Uninstalled");
        return true;
    }

    // Settings are read live every frame, never reinstall (that would tear down the depth swap from the wrong thread)
    void Update() override { pendingReinstall = false; }

    // The card's controls (menu: Image > Depth Blur page). Settings are read live every frame; the change notice only keeps
    // the base class informed and saves.
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        static const Params kDefaults{}; // the registered defaults (changed dots and per-row Reset)
        bool changed = false;
        if (g.gameAaOn) ApexUi::IconNote(IconId::TriangleAlert, "Paused while the game's own Edge Smoothing is on (Options \xE2\x80\xBA Graphics)", VioletTheme::kWarning);
        else if (g.status.rfind("ERROR: ", 0) == 0) ApexUi::IconNote(IconId::TriangleAlert, g.status.c_str() + 7, VioletTheme::kError);

        // Distance: named steps, then fine-tuning (shown as 0-100% of its 0..0.5 range)
        static const char* const kDistances[] = {"Near", "Medium", "Far"};
        static const char* const kDistanceTips[] = {"The blur starts close to the camera", "The default", "Only the far background blurs"};
        static constexpr float kDistanceValues[] = {0.25f, 0.349f, 0.45f};
        int distance = -1; // a fine-tuned value matches none of the steps
        for (int i = 0; i < 3; i++)
            if (std::fabs(g.p.start - kDistanceValues[i]) < 0.0005f) distance = i;
        if (ApexUi::SegmentedRow("Distance", "Where the blur begins", "##Distance", &distance, kDistances, 3, kDistanceTips, nullptr, 1) && distance >= 0) {
            g.p.start = kDistanceValues[distance];
            changed = true;
        }
        {
            ApexUi::SliderOptions o;
            o.format = "%.0f%%";
            o.displayScale = 200.0f;
            o.tooltip = "0% starts at the camera, 100% far away";
            o.defaultValue = kDefaults.start;
            changed |= ApexUi::Slider("Fine-tune distance", &g.p.start, 0.0f, 0.5f, o);
        }
        changed |= ApexUi::SliderPercent("Strength", &g.p.strength, 0.0f, 1.0f, "How blurry the background gets; 0% is no blur", kDefaults.strength);
        changed |= ApexUi::SwitchRow("Sharp in map view", &g.p.offInMapView, "Fades the blur out in map view so lots stay sharp", kDefaults.offInMapView);
        if (g.p.offInMapView && !MapView::Available()) ApexUi::IconNote(IconId::Info, "Map view can't be detected on this game version");

        // The rare knobs of the look
        if (ApexUi::BeginAdvanced("Advanced##DepthBlur")) {
            {
                ApexUi::SliderOptions o;
                o.format = "%.0f%%";
                o.displayScale = 200.0f;
                o.tooltip = "How gradually the blur fades in; lower gives a sharper line";
                o.defaultValue = kDefaults.range;
                changed |= ApexUi::Slider("Transition", &g.p.range, 0.01f, 0.5f, o);
            }
            {
                ApexUi::SliderOptions o;
                o.format = "%.0f%%";
                o.displayScale = 100.0f / 1.5f; // 100% = the default size
                o.tooltip = "How soft the blurred background looks; 100% is the default";
                o.defaultValue = kDefaults.spread;
                changed |= ApexUi::Slider("Blur size", &g.p.spread, 0.5f, 6.0f, o);
            }
            static const char* const kQualities[] = {"Low", "Medium", "High", "Ultra"};
            static const char* const kQualityTips[] = {"Fastest", "Smoother", "The default", "Smoothest large blur; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality", "Higher is smoother and costs a bit more", "##Quality", &g.p.quality, kQualities, 4, kQualityTips, nullptr, kDefaults.quality);
            changed |= ApexUi::SwitchRow("Blur the sky", &g.p.blurSky, "Also blur the sky behind the scenery", kDefaults.blurSky);
            ApexUi::EndAdvanced();
        }
        if (ApexUi::IconTextButton("Reset Depth Blur##DepthBlur", IconId::RotateCcw, "Back to the default distance, strength and look")) {
            ApexUi::ReportChange("Depth Blur reset");
            g.p = Params{};
            changed = true;
        }
        if (changed) NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        ImGui::TextWrapped("Status: %s", g.status.c_str());
        bool changed = false;
        ImGui::SetNextItemWidth(120.0f * ApexUi::Unit());
        if (ImGui::InputFloat("Far plane", &g.p.farPlane, 0.0f, 0.0f, "%.1f")) {
            g.p.farPlane = std::fmin(std::fmax(g.p.farPlane, 10.0f), 10000.0f);
            changed = true;
        }
        ApexUi::Tooltip("Depth linearization: the far plane used to turn the depth buffer into distance (10 - 10000)");
        changed |= ImGui::Checkbox("Show the blur mask", &g.p.debugView);
        ApexUi::Tooltip("Shows the mask instead of the image: white = blurred, black = sharp");
        if (g.ready) ImGui::TextDisabled("Blurred frames: %u  |  blur passes per frame: %d", g.framesBlurred, g.lastIterations);
        ImGui::TextDisabled("Map view: %s  |  fade %.2f", g.mapOpen ? "open" : "closed", g.mapFade);
        if (changed) NotifySettingChanged();
    }
};

APEX_REGISTER_FEATURE(DepthBlurPatch, {.displayName = "Depth Blur",
                                   .description = "Softly blurs the far background, like a camera focused on what's near, while menus stay sharp. "
                                                  "Works with the game's own Edge Smoothing turned off. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                   .category = "Graphics",
                                   .experimental = true,
                                   .supportedVersions = VERSION_ALL,
                                   .technicalDetails = {"Swaps the auto depth-stencil for an INTZ texture via Set/GetDepthStencilSurface detours (transparent to the game).",
                                       "Runs before the first ZENABLE=FALSE backbuffer draw after the scene (bloom composite / UI start).",
                                       "Half-resolution depth-aware separable Gaussian blur (blur factor precomputed in alpha), alpha-blended over the backbuffer.",
                                       "Saves/restores only the states it touches (no full state block).",
                                       "Multisampled depth cannot be sampled in D3D9, so Edge Smoothing must be off."}})
