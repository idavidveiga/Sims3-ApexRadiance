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
#include "d3d9_extra_hooks.h"
#include "depth_share.h"
#include "scene_dither.h"
#include "render_callbacks.h"
#include "post_scene.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
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
sampler2D sDepthLog : register(s5); // log2 of the view distance (APEX_DEPTH_EDGES), point
float4 cRcp    : register(c0); // xy = 1 / screen size
float4 cParams : register(c1); // x = sub-pixel amount, y = edge threshold, z = edge threshold minimum, w = debug view
float4 cSharp  : register(c2); // x = texture sharpening 0..1 (pixels the smoothing left alone)

static const float kStep[STEPS] = { STEP_SIZES };

float Luma(float2 uv) { return dot(tex2Dlod(sColor, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114)); }

// Contrast-adaptive sharpening of one pixel from its 4 neighbours (after AMD FidelityFX CAS, MIT): less where the
// neighbourhood is already contrasty or near black / white, so no halos
float3 Sharpen(float3 c, float3 n, float3 s, float3 w, float3 e, float amount)
{
    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, amount));
    return saturate((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt));
}

// A pixel FXAA leaves as it is: sharpened when asked
float3 Unsmoothed(float2 pos, float3 c, float2 rcp)
{
    [branch] if (cSharp.x > 0.0)
    {
        float3 n = tex2Dlod(sColor, float4(pos - float2(0, rcp.y), 0, 0)).rgb, s = tex2Dlod(sColor, float4(pos + float2(0, rcp.y), 0, 0)).rgb;
        float3 w = tex2Dlod(sColor, float4(pos - float2(rcp.x, 0), 0, 0)).rgb, e = tex2Dlod(sColor, float4(pos + float2(rcp.x, 0), 0, 0)).rgb;
        return Sharpen(c, n, s, w, e, cSharp.x);
    }
    return c;
}

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
#ifdef APEX_DEPTH_EDGES
    // a step in the scene depth next to this pixel (log2 of the view distance, apart by more than DEPTH_STEP): the edge
    // of an object, smoothed down to a lower contrast (walls against walls of the same colour, night scenes)
    float dM = tex2Dlod(sDepthLog, float4(pos, 0, 0)).r;
    float4 dN4 = float4(tex2Dlod(sDepthLog, float4(pos - float2(0, rcp.y), 0, 0)).r, tex2Dlod(sDepthLog, float4(pos + float2(0, rcp.y), 0, 0)).r,
                        tex2Dlod(sDepthLog, float4(pos - float2(rcp.x, 0), 0, 0)).r, tex2Dlod(sDepthLog, float4(pos + float2(rcp.x, 0), 0, 0)).r);
    float depthScale = any(abs(dN4 - dM) > DEPTH_STEP) ? DEPTH_EDGE_SCALE : 1.0;
    [branch] if (range < max(cParams.z, rangeMax * cParams.y) * depthScale)
        return float4(Unsmoothed(pos, rgbM, rcp), 1);
#else
    [branch] if (range < max(cParams.z, rangeMax * cParams.y))
        return float4(Unsmoothed(pos, rgbM, rcp), 1);
#endif

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
// Extreme (30/09, user: "the anti-aliasing still does not leave things perfectly straight, even on Ultra, in both modes"):
// finer first steps and a longer reach (57.5 texels each way against High's 30.5), for long, nearly straight edges at 4K.
constexpr int kFxaaLevels = 4;
constexpr QualityLevel kQualities[kFxaaLevels] = {
    {"5", "1.0, 1.5, 2.0, 4.0, 12.0"},
    {"8", "1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0"},
    {"12", "1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0"},
    {"16", "1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0, 16.0"},
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
sampler2D depthTex  : register(s5); // log2 of the view distance (predication), point

float4 SmaaEdgePS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
#if SMAA_PREDICATION
#ifdef APEX_SMAA_COLOR_EDGES
    return float4(SMAAColorEdgeDetectionPS(uv, offset, colorTex, depthTex), 0, 0);
#else
    return float4(SMAALumaEdgeDetectionPS(uv, offset, colorTex, depthTex), 0, 0);
#endif
#else
#ifdef APEX_SMAA_COLOR_EDGES
    return float4(SMAAColorEdgeDetectionPS(uv, offset, colorTex), 0, 0); // every channel: also edges of equal brightness
#else
    return float4(SMAALumaEdgeDetectionPS(uv, offset, colorTex), 0, 0);
#endif
#endif
}
float4 cSubsample : register(c2); // SMAA T2x: the frame's subsample indices (0 = SMAA 1x)
float4 SmaaWeightPS(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(uv, pixcoord, offset, edgesTex, areaTex, searchTex, cSubsample);
}
float4 SmaaBlendPS(float2 uv : TEXCOORD0) : COLOR0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    float4 c = SMAANeighborhoodBlendingPS(uv, offset, colorTex, blendTex);
    [branch] if (cParams.w > 0.5 || cParams.y > 0.0)
    {
        float4 a = float4(tex2Dlod(blendTex, float4(offset.xy, 0, 0)).a, tex2Dlod(blendTex, float4(offset.zw, 0, 0)).g, tex2Dlod(blendTex, float4(uv, 0, 0)).xz);
        bool blended = dot(a, 1.0) > 1e-5;
        if (!blended && cParams.y > 0.0) // texture sharpening, only where SMAA left the pixel as it was (edges stay smooth)
        {
            float2 r = SMAA_RT_METRICS.xy;
            float3 n = tex2Dlod(colorTex, float4(uv - float2(0, r.y), 0, 0)).rgb, s = tex2Dlod(colorTex, float4(uv + float2(0, r.y), 0, 0)).rgb;
            float3 w = tex2Dlod(colorTex, float4(uv - float2(r.x, 0), 0, 0)).rgb, e = tex2Dlod(colorTex, float4(uv + float2(r.x, 0), 0, 0)).rgb;
            float3 mn = min(c.rgb, min(min(n, s), min(w, e))), mx = max(c.rgb, max(max(n, s), max(w, e)));
            float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
            float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, cParams.y)); // after AMD FidelityFX CAS (MIT)
            c.rgb = saturate((c.rgb + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt));
        }
        if (blended && cParams.w > 0.5) c.rgb = lerp(c.rgb, float3(1, 0, 0), 0.6); // debug: the pixels SMAA blended
    }
    return float4(c.rgb, 1);
}
)HLSL";

// ---- Edges found by depth too (30/09): the scene depth (INTZ, shared by Depth Blur / AO) turned into log2 of the view
// distance, so a step between two pixels is a relative distance (an object in front of another), the same near and far.
// SMAA: the reference's predication (threshold lowered where the depth steps, SMAA_PREDICATION_SCALE 1 so textures keep
// the preset's threshold). FXAA: the same idea on its contrast test.
const char* kDepthLogSource = R"HLSL(
sampler2D sDepth : register(s0); // INTZ, point
float4 cDepth : register(c2);    // x = A, y = 1 / (near A): 1/z = (A - d) / (near A)
float4 LogDepthPS(float2 uv : TEXCOORD0) : COLOR0
{
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    return d >= 0.99999 ? 16.0 : -log2(max(cDepth.x - d, 1e-7) * cDepth.y); // sky: 64 km
}
)HLSL";
// ---- SMAA T2x (30/09): the scene is drawn moved by +-1/4 pixel on alternate frames (SceneBinder, the vertex copies);
// SMAA 1x runs with the matching subsample indices, then this pass blends the frame with the previous frame's SMAA
// output, fetched where the camera saw the same point (the scene depth and M = previous view-projection x inverse of the
// current one, computed on the CPU in double precision). The previous colour is clamped to the 3x3 neighbourhood of the
// current one and dropped where its stored distance differs (a moving object, something uncovered): at most one frame, at
// half weight, can ever show a wrong pixel.
const char* kResolveSource = R"HLSL(
sampler2D sCur     : register(s0); // this frame's SMAA output, point
sampler2D sPrev    : register(s1); // the previous frame's SMAA output, linear
sampler2D sDepth   : register(s2); // INTZ, point
sampler2D sLogPrev : register(s3); // the previous frame's log2 view distance, point
float4 cM[4]  : register(c3);      // rows of M (current NDC + device depth -> previous clip)
float4 cRes   : register(c7);      // x = 1/w, y = 1/h, z = history weight, w = debug view
float4 cRes2  : register(c8);      // x = depth tolerance (log2 units)
float4 ResolvePS(float2 uv : TEXCOORD0) : COLOR0
{
    float3 cur = tex2Dlod(sCur, float4(uv, 0, 0)).rgb;
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float4 p = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 q = float4(dot(cM[0], p), dot(cM[1], p), dot(cM[2], p), dot(cM[3], p));
    float2 prevUv = float2(q.x / q.w * 0.5 + 0.5, 0.5 - q.y / q.w * 0.5);
    float w = cRes.z;
    if (q.w <= 0.0 || any(prevUv < 0.0) || any(prevUv > 1.0)) w = 0.0;
    // the distance the previous frame stored around that point (2x2: the jitter moves silhouettes by a quarter pixel)
    if (d < 0.99999)
    {
        float expect = log2(q.w);
        float2 h = 0.5 * cRes.xy;
        float4 s = float4(tex2Dlod(sLogPrev, float4(prevUv + float2(-h.x, -h.y), 0, 0)).r, tex2Dlod(sLogPrev, float4(prevUv + float2(h.x, -h.y), 0, 0)).r,
                          tex2Dlod(sLogPrev, float4(prevUv + float2(-h.x, h.y), 0, 0)).r, tex2Dlod(sLogPrev, float4(prevUv + float2(h.x, h.y), 0, 0)).r);
        float4 e = abs(s - expect);
        // the tolerance grows with the slope of the surface there (far ground seen at a grazing angle changes distance fast)
        if (min(min(e.x, e.y), min(e.z, e.w)) > cRes2.x + 1.5 * (max(max(s.x, s.y), max(s.z, s.w)) - min(min(s.x, s.y), min(s.z, s.w)))) w = 0.0;
    }
    // the 3x3 neighbourhood of the current frame bounds the previous colour
    float3 mn = cur, mx = cur;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float3 c = tex2Dlod(sCur, float4(uv + float2(x, y) * cRes.xy, 0, 0)).rgb;
            mn = min(mn, c);
            mx = max(mx, c);
        }
    float3 prev = clamp(tex2Dlod(sPrev, float4(prevUv, 0, 0)).rgb, mn, mx);
    float3 o = lerp(cur, prev, w);
    if (cRes.w > 0.5) o = lerp(o, w > 0.0 ? float3(0, 0.6, 0) : float3(0.8, 0, 0.8), 0.35); // debug: green = blended, magenta = history dropped
    return float4(o, 1);
}
)HLSL";

constexpr const char* kDepthStep = "0.02";      // log2 units: a 1.4% jump in distance between neighbours = an object edge
constexpr const char* kDepthEdgeScale = "0.4"; // the contrast needed there (x the usual threshold)

// the reference presets (SMAA.hlsl, "SMAA Presets"), with the threshold as a shader constant
struct SmaaPreset {
    float threshold;
    const char* steps;
    const char* stepsDiag; // nullptr = diagonal and corner detection off
    bool colorEdges;       // the reference's colour edge detection instead of luma
};
// Extreme (30/09, beyond the reference presets, within its ranges): at 4K one step of a nearly horizontal edge (a roof, a
// floor line) can be longer than Ultra's reach (32 search steps, 2 pixels each, per side), and SMAA then leaves it jagged;
// 112 steps (the reference's maximum) and 20 diagonal steps (its maximum) follow such edges, and the colour edge detection
// also catches edges between colours of the same brightness, which the luma detection misses
constexpr int kSmaaLevels = 5;
constexpr SmaaPreset kSmaaPresets[kSmaaLevels] = {{0.15f, "4", nullptr, false}, {0.1f, "8", nullptr, false}, {0.1f, "16", "8", false},
                                                  {0.05f, "32", "16", false}, {0.05f, "112", "20", true}};

// ---- every variant compiled at start-up on a background thread (framework/shader_cache.h) ----
// The render thread only creates the shader objects from the kept bytecode (first use, and after ReleaseShaders).
// Priority 0 = the default FXAA quality (Balanced) and SMAA preset (High).
ShaderCache::Id AddFxaa(int q, const char* tag, bool depth = false) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kShaderSource;
    d.sourceName = "edge_smoothing.hlsl";
    d.entry = "FxaaPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.macros = {{"STEPS", kQualities[q].steps}, {"STEP_SIZES", kQualities[q].sizes}};
    if (depth) {
        d.macros.emplace_back("APEX_DEPTH_EDGES", "1");
        d.macros.emplace_back("DEPTH_STEP", kDepthStep);
        d.macros.emplace_back("DEPTH_EDGE_SCALE", kDepthEdgeScale);
    }
    d.priority = q == 1 ? 0 : 1;
    return ShaderCache::Add(std::move(d));
}
ShaderCache::Id AddDepthLog() {
    ShaderCache::Desc d;
    d.tag = "EdgeSmoothing depth (log2)";
    d.source = kDepthLogSource;
    d.sourceName = "edge_smoothing_depth.hlsl";
    d.entry = "LogDepthPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 0;
    return ShaderCache::Add(std::move(d));
}
ShaderCache::Id AddResolve() {
    ShaderCache::Desc d;
    d.tag = "EdgeSmoothing SMAA T2x resolve";
    d.source = kResolveSource;
    d.sourceName = "edge_smoothing_resolve.hlsl";
    d.entry = "ResolvePS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}
// pass 0..2 = edges, weights, blend; pass 3 = the edge pass with depth predication
ShaderCache::Id AddSmaa(int q, int pass, const char* tag) {
    static const char* const kEntries[3] = {"SmaaEdgePS", "SmaaWeightPS", "SmaaBlendPS"};
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = std::string(kSmaaPrefix) + reinterpret_cast<const char*>(kSmaaHlsl) + kSmaaSuffix;
    d.sourceName = "SMAA.hlsl";
    d.entry = kEntries[pass == 3 ? 0 : pass];
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.macros.emplace_back("SMAA_MAX_SEARCH_STEPS", kSmaaPresets[q].steps);
    if (pass == 3) {
        d.macros.emplace_back("SMAA_PREDICATION", "1");
        d.macros.emplace_back("SMAA_PREDICATION_THRESHOLD", kDepthStep);
        d.macros.emplace_back("SMAA_PREDICATION_SCALE", "1.0");
        d.macros.emplace_back("SMAA_PREDICATION_STRENGTH", "0.6"); // threshold x 0.4 where the depth steps
    }
    if (kSmaaPresets[q].stepsDiag) {
        d.macros.emplace_back("SMAA_MAX_SEARCH_STEPS_DIAG", kSmaaPresets[q].stepsDiag);
        d.macros.emplace_back("SMAA_CORNER_ROUNDING", "25");
    } else {
        d.macros.emplace_back("SMAA_DISABLE_DIAG_DETECTION", "1");
        d.macros.emplace_back("SMAA_DISABLE_CORNER_DETECTION", "1");
    }
    if (kSmaaPresets[q].colorEdges) d.macros.emplace_back("APEX_SMAA_COLOR_EDGES", "1");
    d.priority = q == 2 ? 0 : 1;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kFxaaPsId[kFxaaLevels] = {AddFxaa(0, "EdgeSmoothing FXAA (Fast)"), AddFxaa(1, "EdgeSmoothing FXAA (Balanced)"), AddFxaa(2, "EdgeSmoothing FXAA (High)"),
                                                AddFxaa(3, "EdgeSmoothing FXAA (Extreme)")};
const ShaderCache::Id kSmaaPsId[kSmaaLevels][3] = {
    {AddSmaa(0, 0, "EdgeSmoothing SMAA edges (Low)"), AddSmaa(0, 1, "EdgeSmoothing SMAA weights (Low)"), AddSmaa(0, 2, "EdgeSmoothing SMAA blend (Low)")},
    {AddSmaa(1, 0, "EdgeSmoothing SMAA edges (Medium)"), AddSmaa(1, 1, "EdgeSmoothing SMAA weights (Medium)"), AddSmaa(1, 2, "EdgeSmoothing SMAA blend (Medium)")},
    {AddSmaa(2, 0, "EdgeSmoothing SMAA edges (High)"), AddSmaa(2, 1, "EdgeSmoothing SMAA weights (High)"), AddSmaa(2, 2, "EdgeSmoothing SMAA blend (High)")},
    {AddSmaa(3, 0, "EdgeSmoothing SMAA edges (Ultra)"), AddSmaa(3, 1, "EdgeSmoothing SMAA weights (Ultra)"), AddSmaa(3, 2, "EdgeSmoothing SMAA blend (Ultra)")},
    {AddSmaa(4, 0, "EdgeSmoothing SMAA edges (Extreme)"), AddSmaa(4, 1, "EdgeSmoothing SMAA weights (Extreme)"), AddSmaa(4, 2, "EdgeSmoothing SMAA blend (Extreme)")},
};
const ShaderCache::Id kSmaaDepthEdgeId[kSmaaLevels] = {AddSmaa(0, 3, "EdgeSmoothing SMAA edges + depth (Low)"), AddSmaa(1, 3, "EdgeSmoothing SMAA edges + depth (Medium)"),
                                                       AddSmaa(2, 3, "EdgeSmoothing SMAA edges + depth (High)"), AddSmaa(3, 3, "EdgeSmoothing SMAA edges + depth (Ultra)"),
                                                       AddSmaa(4, 3, "EdgeSmoothing SMAA edges + depth (Extreme)")};
const ShaderCache::Id kFxaaDepthId[kFxaaLevels] = {AddFxaa(0, "EdgeSmoothing FXAA + depth (Fast)", true), AddFxaa(1, "EdgeSmoothing FXAA + depth (Balanced)", true),
                                                   AddFxaa(2, "EdgeSmoothing FXAA + depth (High)", true), AddFxaa(3, "EdgeSmoothing FXAA + depth (Extreme)", true)};
const ShaderCache::Id kDepthLogId = AddDepthLog();
const ShaderCache::Id kResolveId = AddResolve();

struct Params {
    int method = 1;           // 0 FXAA, 1 SMAA
    int quality = 1;          // FXAA: 0 fast, 1 balanced, 2 high
    int smaaQuality = 2;      // SMAA: 0 low, 1 medium, 2 high, 3 ultra, 4 extreme
    float subpix = 0.5f;      // sub-pixel smoothing (thin lines, texture detail): 0 = off, 1 = soft
    float sensitivity = 0.125f; // edge threshold: lower = more edges smoothed
    bool depthEdges = true;   // also find object edges in the scene depth (fainter edges of objects smoothed)
    float sharpen = 0.0f;     // texture sharpening of the pixels the smoothing left alone, 0..1 (0 = off)
    bool temporal = false;    // SMAA T2x: the scene drawn with a quarter-pixel jitter, blended with the previous frame
    bool debugTemporal = false; // developer: green = blended with the previous frame, magenta = history dropped
    bool swapJitter = false;    // developer: the other pairing of jitter and subsample indices
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
    IDirect3DPixelShader9* ps[kFxaaLevels] = {};
    bool compileTried[kFxaaLevels] = {};
    // SMAA
    IDirect3DTexture9 *edgesTex = nullptr, *blendTex = nullptr, *areaTex = nullptr, *searchTex = nullptr;
    IDirect3DSurface9 *edgesSurf = nullptr, *blendSurf = nullptr;
    IDirect3DPixelShader9* smaaPs[kSmaaLevels][3] = {};
    bool smaaTried[kSmaaLevels] = {};
    // depth edges
    IDirect3DTexture9* depthLogTex = nullptr;
    IDirect3DSurface9* depthLogSurf = nullptr;
    IDirect3DPixelShader9* psDepthLog = nullptr;
    IDirect3DPixelShader9* smaaDepthEdgePs[kSmaaLevels] = {};
    IDirect3DPixelShader9* fxaaDepthPs[kFxaaLevels] = {};
    bool depthShadersTried = false;
    bool depthRequested = false; // DepthShare::Request(true) held
    bool depthUsed = false;      // the last frame's pass used the depth
    unsigned framesWithDepth = 0;
    // SMAA T2x
    IDirect3DTexture9 *histTex[2] = {}, *logPrevTex = nullptr; // [cur] this frame's SMAA output, [1 - cur] the previous one
    IDirect3DSurface9 *histSurf[2] = {}, *logPrevSurf = nullptr;
    int histCur = 0;
    IDirect3DPixelShader9* psResolve = nullptr;
    bool resolveTried = false;
    bool temporalHeld = false;   // SceneBinder::AcquireJitter + PostScene::WantCamera held
    bool historyValid = false;   // the previous frame left a usable history (its SMAA output, depth and camera)
    double prevVp[4][4] = {};
    int jitterIndex = -1;        // the jitter the scene of this frame was drawn with (0 / 1), -1 none
    unsigned stillFrames = 0;    // frames in a row the camera did not move
    bool ranThisFrame = false;
    unsigned framesBlended = 0, historyResets = 0;
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
    SafeRelease(g.depthLogSurf);
    SafeRelease(g.depthLogTex);
    for (int i = 0; i < 2; i++) {
        SafeRelease(g.histSurf[i]);
        SafeRelease(g.histTex[i]);
    }
    SafeRelease(g.logPrevSurf);
    SafeRelease(g.logPrevTex);
    g.historyValid = false;
    for (int i = 0; i < AaState::kQ; i++) {
        SafeRelease(g.qDisjoint[i]);
        SafeRelease(g.qBegin[i]);
        SafeRelease(g.qEnd[i]);
        SafeRelease(g.qFreq[i]);
        g.qIssued[i] = false;
    }
}

void ReleaseShaders() {
    for (int i = 0; i < kFxaaLevels; i++) {
        SafeRelease(g.ps[i]);
        g.compileTried[i] = false;
    }
    for (int q = 0; q < kSmaaLevels; q++) {
        for (auto& ps : g.smaaPs[q]) SafeRelease(ps);
        g.smaaTried[q] = false;
        SafeRelease(g.smaaDepthEdgePs[q]);
    }
    for (auto& ps : g.fxaaDepthPs) SafeRelease(ps);
    SafeRelease(g.psDepthLog);
    g.depthShadersTried = false;
    SafeRelease(g.psResolve);
    g.resolveTried = false;
}

// The depth-edge shaders (all variants, created on first use from the precompiled bytecode). False when any is missing:
// the effect then runs without the depth.
bool DepthShaders(IDirect3DDevice9* dev) {
    if (!g.depthShadersTried) {
        g.depthShadersTried = true;
        auto make = [&](ShaderCache::Id id, IDirect3DPixelShader9** ps, const char* what) {
            std::string msg;
            if (ShaderCache::CreatePixelShader(dev, id, ps, &msg) == ShaderCache::Result::CompileFailed)
                LOG_ERROR(std::format("[EdgeSmoothing] {} failed to compile: {}", what, msg));
        };
        make(kDepthLogId, &g.psDepthLog, "LogDepthPS");
        for (int q = 0; q < kSmaaLevels; q++) make(kSmaaDepthEdgeId[q], &g.smaaDepthEdgePs[q], "SMAA edges + depth");
        for (int q = 0; q < kFxaaLevels; q++) make(kFxaaDepthId[q], &g.fxaaDepthPs[q], "FXAA + depth");
    }
    if (!g.psDepthLog) return false;
    for (auto* ps : g.smaaDepthEdgePs)
        if (!ps) return false;
    for (auto* ps : g.fxaaDepthPs)
        if (!ps) return false;
    return true;
}

// The three SMAA passes of one preset (created on first use from the precompiled bytecode)
bool SmaaShaders(IDirect3DDevice9* dev, int q) {
    q = std::clamp(q, 0, kSmaaLevels - 1);
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
    q = std::clamp(q, 0, kFxaaLevels - 1);
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
    // the depth-edge target (optional: without it the effect runs on colour alone)
    if (FAILED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.depthLogTex, nullptr)) || !g.depthLogTex ||
        FAILED(g.depthLogTex->GetSurfaceLevel(0, &g.depthLogSurf)) || !g.depthLogSurf) {
        SafeRelease(g.depthLogSurf);
        SafeRelease(g.depthLogTex);
        LOG_WARNING("[EdgeSmoothing] No video memory for the depth-edge target: edges from colour only");
    }
    // SMAA T2x history (optional: without it the temporal blend stays off)
    bool histOk = g.depthLogTex != nullptr;
    for (int i = 0; i < 2 && histOk; i++)
        histOk = SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g.histTex[i], nullptr)) && g.histTex[i] &&
                 SUCCEEDED(g.histTex[i]->GetSurfaceLevel(0, &g.histSurf[i])) && g.histSurf[i];
    histOk = histOk && SUCCEEDED(dev->CreateTexture(g.width, g.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.logPrevTex, nullptr)) && g.logPrevTex &&
             SUCCEEDED(g.logPrevTex->GetSurfaceLevel(0, &g.logPrevSurf)) && g.logPrevSurf;
    if (!histOk) {
        for (int i = 0; i < 2; i++) {
            SafeRelease(g.histSurf[i]);
            SafeRelease(g.histTex[i]);
        }
        SafeRelease(g.logPrevSurf);
        SafeRelease(g.logPrevTex);
        LOG_WARNING("[EdgeSmoothing] No video memory for the temporal history: SMAA T2x unavailable");
    }
    g.historyValid = false;
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
constexpr UINT kPSConsts = 3;

// The depth-edge pass: the scene depth (INTZ, bound as the depth-stencil right now) -> log2 of the view distance in
// depthLogTex. True when it ran (the AA passes then read it at s5). Saves and restores everything it touches.
bool TemporalWanted(); // SMAA T2x on (below)
bool RunDepthPass(IDirect3DDevice9* dev) {
    if (!(g.p.depthEdges || TemporalWanted()) || !g.depthLogSurf) return false;
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!depth || !DepthShaders(dev)) return false;
    IDirect3DSurface9* ds = nullptr; // the scene depth must be the one bound now (not a reflection or UI pass), as AO checks
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    const bool sceneDepth = ds && ds == DepthShare::Surface();
    if (ds) ds->Release();
    if (!sceneDepth) return false;
    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD rs[kRS], ss[kSS];
    float oldConst[4];
    D3DVIEWPORT9 oldVp{};
    dev->GetRenderTarget(0, &oldRt);
    dev->GetPixelShader(&oldPs);
    dev->GetVertexShader(&oldVs);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFvf);
    dev->GetStreamSource(0, &oldStream, &oldOffset, &oldStride);
    dev->GetTexture(0, &oldTex);
    for (int i = 0; i < kRS; i++) dev->GetRenderState(kRenderStates[i], &rs[i]);
    for (int i = 0; i < kSS; i++) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
    dev->GetPixelShaderConstantF(2, oldConst, 1);
    dev->GetViewport(&oldVp);

    dev->SetRenderTarget(0, g.depthLogSurf);
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
    dev->SetTexture(0, depth);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    const float nearZ = PostScene::CameraNear() > 0.0f ? PostScene::CameraNear() : 0.25f, A = PostScene::CameraDepthA();
    const float c[4] = {A, 1.0f / (nearZ * A), 0, 0};
    dev->SetPixelShaderConstantF(2, c, 1);
    dev->SetPixelShader(g.psDepthLog);
    const float x1 = static_cast<float>(g.width) - 0.5f, y1 = static_cast<float>(g.height) - 0.5f;
    const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));

    dev->SetRenderTarget(0, oldRt); // resets the viewport, restored below
    dev->SetTexture(0, oldTex);
    for (int i = 0; i < kSS; i++) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(2, oldConst, 1);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride); // DrawPrimitiveUP clears stream 0
    dev->SetViewport(&oldVp);
    SafeRelease(oldRt);
    SafeRelease(oldTex);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    return true;
}

// Binds (or restores) sampler 5 for the depth-edge variants
struct DepthSampler {
    IDirect3DDevice9* dev;
    bool on;
    IDirect3DBaseTexture9* oldTex = nullptr;
    DWORD ss[kSS] = {};
    DepthSampler(IDirect3DDevice9* d, bool use) : dev(d), on(use) {
        if (!on) return;
        dev->GetTexture(5, &oldTex);
        for (int i = 0; i < kSS; i++) dev->GetSamplerState(5, kSamplerStates[i], &ss[i]);
        dev->SetTexture(5, g.depthLogTex);
        dev->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(5, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(5, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(5, D3DSAMP_MIPMAPLODBIAS, 0);
    }
    ~DepthSampler() {
        if (!on) return;
        dev->SetTexture(5, oldTex);
        for (int i = 0; i < kSS; i++) dev->SetSamplerState(5, kSamplerStates[i], ss[i]);
        SafeRelease(oldTex);
    }
};

void RunFxaa(IDirect3DDevice9* dev, bool useDepth) {
    IDirect3DPixelShader9* ps = useDepth ? g.fxaaDepthPs[std::clamp(g.p.quality, 0, kFxaaLevels - 1)] : ShaderFor(dev, g.p.quality);
    if (!ps) return;
    DepthSampler depthSampler(dev, useDepth);
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
                                   {g.p.subpix, g.p.sensitivity, g.p.sensitivity / 3.0f, g.p.debugView ? 1.0f : 0.0f},
                                   {std::clamp(g.p.sharpen, 0.0f, 1.0f), 0, 0, 0}};
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
void RunSmaa(IDirect3DDevice9* dev, bool useDepth, const float subsample[4]) {
    const int q = std::clamp(g.p.smaaQuality, 0, kSmaaLevels - 1);
    if (!SmaaShaders(dev, q)) return;
    DepthSampler depthSampler(dev, useDepth);
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
    const float c[kPSConsts][4] = {{1.0f / W, 1.0f / H, W, H}, {kSmaaPresets[q].threshold, std::clamp(g.p.sharpen, 0.0f, 1.0f), 0, g.p.debugView ? 1.0f : 0.0f}, {subsample[0], subsample[1], subsample[2], subsample[3]}};
    dev->SetPixelShaderConstantF(0, &c[0][0], kPSConsts);

    // 1. edges (the targets are cleared every frame, alpha too: the passes discard where there is nothing to do)
    dev->SetRenderTarget(0, g.edgesSurf);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetTexture(0, g.copyTex);
    dev->SetPixelShader(useDepth ? g.smaaDepthEdgePs[q] : g.smaaPs[q][0]);
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

// ---- SMAA T2x: jitter sequence, camera reprojection, history ----
// The reference's T2x pairs (SMAA.h / its demo): the jitter in pixels and the subsample indices of the weights pass
constexpr float kT2xJitter[2][2] = {{0.25f, -0.25f}, {-0.25f, 0.25f}};
constexpr float kT2xSubsample[2][4] = {{1, 1, 1, 0}, {2, 2, 2, 0}};
constexpr float kT2xWeight = 0.5f;    // the previous frame's share where it is kept
constexpr float kT2xDepthTol = 0.03f; // log2 units: the stored distance may differ by ~2%
constexpr double kCutNdc = 0.3;       // the screen centre moved more than this (NDC) since the last frame: a camera cut

bool TemporalWanted() { return g.active && g.p.method == 1 && g.p.temporal; }

// 4x4 inverse (Gauss-Jordan, partial pivoting), false when singular
bool Invert(const double a[4][4], double out[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) m[r][c] = c < 4 ? a[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::abs(m[r][c]) > std::abs(m[p][c])) p = r;
        if (std::abs(m[p][c]) < 1e-12) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) std::swap(m[p][k], m[c][k]);
        const double inv = 1.0 / m[c][c];
        for (int k = 0; k < 8; k++) m[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = m[r][c];
            for (int k = 0; k < 8; k++) m[r][k] -= f * m[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r][c] = m[r][c + 4];
    return true;
}

// This frame's SMAA output (the back buffer now) becomes the next frame's history
void StoreHistory(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    dev->StretchRect(bb, nullptr, g.histSurf[g.histCur], nullptr, D3DTEXF_NONE);
    bb->Release();
}

// The blend with the previous frame into the back buffer (reads histTex[histCur] = this frame, histTex[1 - histCur] = the
// previous one). Saves and restores what it touches.
void RunResolve(IDirect3DDevice9* dev, const double M[4][4]) {
    if (!g.resolveTried) {
        g.resolveTried = true;
        std::string msg;
        if (ShaderCache::CreatePixelShader(dev, kResolveId, &g.psResolve, &msg) == ShaderCache::Result::CompileFailed)
            LOG_ERROR("[EdgeSmoothing] T2x resolve failed to compile: " + msg);
    }
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!g.psResolve || !depth) return;
    constexpr DWORD kSamplers = 4;
    constexpr UINT kC0 = 3, kCN = 6; // c3..c8
    IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DPixelShader9* oldPs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFvf = 0;
    IDirect3DVertexBuffer9* oldStream = nullptr;
    UINT oldOffset = 0, oldStride = 0;
    IDirect3DBaseTexture9* oldTex[kSamplers] = {};
    DWORD rs[kRS], ss[kSamplers][kSS];
    float oldConst[kCN * 4];
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
    dev->GetPixelShaderConstantF(kC0, oldConst, kCN);
    dev->GetViewport(&oldVp);

    dev->SetRenderTarget(0, bb);
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
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE); // alpha: the bloom mask
    IDirect3DBaseTexture9* tex[kSamplers] = {g.histTex[g.histCur], g.histTex[1 - g.histCur], depth, g.logPrevTex};
    for (DWORD s = 0; s < kSamplers; s++) {
        const DWORD f = s == 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        dev->SetTexture(s, tex[s]);
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }
    float c[kCN][4] = {};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c[r][k] = static_cast<float>(M[r][k]);
    c[4][0] = 1.0f / static_cast<float>(g.width);
    c[4][1] = 1.0f / static_cast<float>(g.height);
    c[4][2] = kT2xWeight;
    c[4][3] = g.p.debugTemporal ? 1.0f : 0.0f;
    c[5][0] = kT2xDepthTol;
    dev->SetPixelShaderConstantF(kC0, &c[0][0], kCN);
    dev->SetPixelShader(g.psResolve);
    DrawQuad(dev);

    dev->SetRenderTarget(0, oldRt);
    for (DWORD s = 0; s < kSamplers; s++) {
        dev->SetTexture(s, oldTex[s]);
        for (int i = 0; i < kSS; i++) dev->SetSamplerState(s, kSamplerStates[i], ss[s][i]);
        SafeRelease(oldTex[s]);
    }
    for (int i = 0; i < kRS; i++) dev->SetRenderState(kRenderStates[i], rs[i]);
    dev->SetPixelShaderConstantF(kC0, oldConst, kCN);
    dev->SetPixelShader(oldPs);
    dev->SetVertexShader(oldVs);
    if (oldDecl) dev->SetVertexDeclaration(oldDecl);
    else dev->SetFVF(oldFvf);
    dev->SetStreamSource(0, oldStream, oldOffset, oldStride);
    dev->SetViewport(&oldVp);
    SafeRelease(oldRt);
    SafeRelease(oldPs);
    SafeRelease(oldVs);
    SafeRelease(oldDecl);
    SafeRelease(oldStream);
    bb->Release();
}

// After SMAA (T2x on, depth pass done): blend with the history when it is usable, then keep this frame as the next history
void TemporalStep(IDirect3DDevice9* dev, bool haveVp, const float vpNow[4][4]) {
    if (!haveVp) {
        g.historyValid = false;
        return;
    }
    double cur[4][4], inv[4][4], M[4][4];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) cur[r][k] = vpNow[r][k];
    bool blend = g.historyValid && Invert(cur, inv);
    if (blend) {
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                double s = 0;
                for (int j = 0; j < 4; j++) s += g.prevVp[r][j] * inv[j][k];
                M[r][k] = s;
            }
        // a camera cut: the screen centre (mid depth) lands far from where it was
        const double qx = M[0][2] * 0.5 + M[0][3], qy = M[1][2] * 0.5 + M[1][3], qw = M[3][2] * 0.5 + M[3][3];
        if (qw <= 1e-9 || std::abs(qx / qw) > kCutNdc || std::abs(qy / qw) > kCutNdc) blend = false;
    }
    // the camera did not move since the last frame (M = identity): the jitter stops (see FxaaEffect)
    bool still = blend;
    for (int r = 0; r < 4 && still; r++)
        for (int k = 0; k < 4 && still; k++) still = std::abs(M[r][k] - (r == k ? 1.0 : 0.0)) < 1e-5;
    g.stillFrames = still ? g.stillFrames + 1 : 0;
    if (still && g.jitterIndex < 0) blend = false; // still and not moved: the frame is SMAA 1x as it is (nothing to blend, no ghosts)
    StoreHistory(dev); // this frame's SMAA output, before the blend writes the back buffer
    if (blend) {
        RunResolve(dev, M);
        g.framesBlended++;
    } else if (g.historyValid) {
        g.historyResets++;
    }
    // the next frame: this frame is its history (SMAA output, log2 depth, camera)
    dev->StretchRect(g.depthLogSurf, nullptr, g.logPrevSurf, nullptr, D3DTEXF_NONE);
    g.histCur = 1 - g.histCur;
    std::memcpy(g.prevVp, cur, sizeof cur);
    g.historyValid = true;
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
    const bool depthRan = RunDepthPass(dev);
    const bool useDepth = depthRan && g.p.depthEdges;
    g.depthUsed = depthRan;
    if (depthRan) g.framesWithDepth++;
    g.ranThisFrame = true;
    const bool t2x = method == 1 && TemporalWanted() && depthRan && g.histTex[0] && g.logPrevTex;
    float vp[4][4] = {};
    const bool haveVp = t2x && PostScene::CameraViewProj(vp);
    static const float kNoSubsample[4] = {0, 0, 0, 0};
    const int j = g.jitterIndex; // the jitter this frame's scene was drawn with
    const float* subsample = (t2x && j >= 0) ? kT2xSubsample[g.p.swapJitter ? 1 - j : j] : kNoSubsample;
    if (method == 1) RunSmaa(dev, useDepth, subsample);
    else RunFxaa(dev, useDepth);
    if (t2x) TemporalStep(dev, haveVp, vp);
    else g.historyValid = false;
    // the next frame's jitter: only while the blend can run (else the image would shake)
    // ... and only while the camera moves: still, SMAA 1x alone is already stable, and a still image must never shake
    // (30/09, user: "at a distance it seems to keep moving even when still")
    if (t2x && haveVp && g.stillFrames < 2) {
        g.jitterIndex = j == 0 ? 1 : 0;
        SceneBinder::SetFrameJitter(true, 2.0f * kT2xJitter[g.jitterIndex][0] / static_cast<float>(g.width),
                                    2.0f * kT2xJitter[g.jitterIndex][1] / static_cast<float>(g.height));
    } else {
        g.jitterIndex = -1;
        SceneBinder::SetFrameJitter(false, 0, 0);
    }
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
    // the INTZ depth swap runs while the depth edges or SMAA T2x want it (it also runs for Depth Blur and AO)
    const bool wantDepth = g.p.depthEdges || TemporalWanted();
    if (wantDepth != g.depthRequested) {
        DepthShare::Request(wantDepth);
        g.depthRequested = wantDepth;
    }
    // SMAA T2x: the jittered vertex copies and the camera while it is on
    if (TemporalWanted() != g.temporalHeld) {
        g.temporalHeld = !g.temporalHeld;
        if (g.temporalHeld) {
            SceneBinder::AcquireJitter();
            PostScene::WantCamera(true);
        } else {
            SceneBinder::ReleaseJitter();
            PostScene::WantCamera(false);
            g.jitterIndex = -1;
            g.historyValid = false;
        }
    }
    // a frame without the scene (loading, a menu over a black screen): no jitter and no history until it comes back
    if (!g.ranThisFrame && g.jitterIndex >= 0) {
        g.jitterIndex = -1;
        g.historyValid = false;
        SceneBinder::SetFrameJitter(false, 0, 0);
    }
    g.ranThisFrame = false;
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
                            "SMAA: the reference presets, and Extreme. High and above also handle diagonals and corners; Ultra catches fainter edges "
                            "(good at night); Extreme follows very long edges (4K) and colour edges.",
                            {"Low", "Medium", "High", "Ultra", "Extreme"});
        RegisterEnumSetting(&g.p.quality, "qualidade", 1,
                            "FXAA: how far each edge is followed. Higher = smoother long, nearly straight edges, costs a little more.",
                            {"Fast", "Balanced", "High", "Extreme"});
        RegisterFloatSetting(&g.p.subpix, "suavidade", SettingWidget::Slider, 0.5f, 0.0f, 1.0f,
                             "Also smooths thin lines and sub-pixel detail. Higher = smoother, but textures get slightly softer.");
        RegisterFloatSetting(&g.p.sensitivity, "sensibilidade", SettingWidget::Slider, 0.125f, 0.063f, 0.333f,
                             "Minimum contrast for an edge to be smoothed. Lower = catches more edges (also in dark night scenes).");
        RegisterBoolSetting(&g.p.depthEdges, "depthEdges", true,
                            "Also finds the edges of objects from the scene depth, so edges with little contrast (walls against walls of the same colour, "
                            "night scenes) are smoothed too, while textures stay sharp.");
        RegisterBoolSetting(&g.p.temporal, "temporal", false,
                            "SMAA T2x: the world is drawn a quarter pixel apart on alternate frames and each frame is blended with the last one, so thin "
                            "lines and far edges shimmer much less. Experimental.");
        RegisterFloatSetting(&g.p.sharpen, "sharpen", SettingWidget::Slider, 0.0f, 0.0f, 1.0f,
                             "Sharpens the textures the smoothing leaves alone (the smoothed edges stay smooth). 0% is off.");
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
        if (g.depthRequested) DepthShare::Request(false);
        g.depthRequested = false;
        if (g.temporalHeld) {
            SceneBinder::SetFrameJitter(false, 0, 0);
            SceneBinder::ReleaseJitter();
            PostScene::WantCamera(false);
            g.temporalHeld = false;
        }
        g.jitterIndex = -1;
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
            static const char* const kSmaa[] = {"Low", "Medium", "High", "Ultra", "Extreme"};
            static const char* const kSmaaTips[] = {"Fastest; smooths the clearest edges", "A good balance", "Also smooths diagonals and corners",
                                                    "Catches faint edges too, great at night", "Straightest long edges, best at 4K; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality##Smaa", "Higher smooths more edges and costs a bit more", "##SmaaQuality", &g.p.smaaQuality, kSmaa, 5, kSmaaTips, nullptr, kDefaults.smaaQuality);
            ApexUi::SetNextRowBadge("Experimental", "Still being tested: if anything looks wrong or the game crashes, turn it off");
            changed |= ApexUi::SwitchRow("Temporal smoothing", &g.p.temporal, "Also blends each frame with the last one: thin lines and far edges shimmer much less",
                                         kDefaults.temporal);
        } else {
            static const char* const kFxaa[] = {"Fast", "Balanced", "High", "Extreme"};
            static const char* const kFxaaTips[] = {"Fastest", "A good balance", "Smoother long edges; costs a little more", "Straightest long edges, best at 4K; costs the most"};
            changed |= ApexUi::SegmentedRow("Quality##Fxaa", "Higher smooths more edges and costs a bit more", "##FxaaQuality", &g.p.quality, kFxaa, 4, kFxaaTips, nullptr, kDefaults.quality);
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
        // both methods (30/09): object edges from the scene depth, and texture sharpening where nothing was smoothed
        changed |= ApexUi::SwitchRow("Edges from depth", &g.p.depthEdges, "Also smooths faint edges of objects, while textures stay sharp", kDefaults.depthEdges);
        if (g.p.depthEdges && g.ready && g.framesSmoothed > 60 && !g.depthUsed)
            ApexUi::IconNote(IconId::Info, "The scene depth is not available right now: edges come from colour only");
        changed |= ApexUi::SliderPercent("Sharpen textures", &g.p.sharpen, 0.0f, 1.0f, "Crisper textures; the smoothed edges stay smooth. 0% is off", kDefaults.sharpen);
        // 30/09 (user: "many players at 1080p showed the game very jagged", with this on): at 1200 lines or fewer, the driver's
        // own supersampling (render at a higher resolution, shown on the same screen) smooths what no post-process AA can
        // (thin rails, wires, leaves)
        if (g.height > 0 && g.height <= 1200) {
            ApexUi::IconNote(IconId::Info, "Smoothest at 1080p: NVIDIA DSR or AMD VSR with a higher game resolution");
            ApexUi::Tooltip("Turn it on in the NVIDIA Control Panel (DSR) or AMD Software (VSR), then pick 1440p or 4K in the game; "
                            "it costs more and the game's interface gets smaller");
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
        if (g.ready) ImGui::TextDisabled("Frames smoothed: %u (with the scene depth %u)", g.framesSmoothed, g.framesWithDepth);
        if (g.p.temporal) {
            const SceneBinder::Coverage cov = SceneBinder::LastCoverage();
            ImGui::TextDisabled("SMAA T2x: %u frames blended, %u history resets, jitter %d; last frame: %u scene draws moved, %u vertex shaders refused, %u fixed-function",
                                g.framesBlended, g.historyResets, g.jitterIndex, cov.jittered, cov.refused, cov.noShader);
            changed |= ImGui::Checkbox("Show the temporal blend (green = blended, magenta = history dropped)", &g.p.debugTemporal);
            changed |= ImGui::Checkbox("Swap jitter and subsample pairing", &g.p.swapJitter);
        }
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
