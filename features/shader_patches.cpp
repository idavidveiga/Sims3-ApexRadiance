// Bytecode edits of the game's shaders (part of Night Lighting). The game's shaders are found by
// pattern (instruction and register shape), not by exact bytes, so variants of the same shader are covered too. Each
// edit inserts a few instructions, uses the first free temp register / sampler / constant, and fails (leaving the shader
// alone) when the expected pattern is not there. Findings behind each one: NOTAS-ILUMINACAO.md.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "shader_patches.h"
#include <algorithm>

namespace {

// ---- token helpers (D3D9 SM2/SM3) ----
enum : DWORD { kTemp = 0, kInput = 1, kConst = 2, kTexture = 3, kOutput = 6, kColorOut = 8, kSampler = 10 };
enum : DWORD { kMov = 0x01, kAdd = 0x02, kMova = 0x2E, kMad = 0x04, kMul = 0x05, kDp3 = 0x08, kDp4 = 0x09, kMax = 0x0B, kSlt = 0x0C, kLrp = 0x12, kTexkill = 0x41, kDcl = 0x1F, kDefB = 0x2F, kDefI = 0x30, kDef = 0x51, kTexld = 0x42 };
constexpr DWORD kSwzX = 0x00, kSwzY = 0x55, kSwzW = 0xFF, kSwzXYZW = 0xE4, kSwzZWZW = 0xEE, kSwzXYXZ = 0x84, kSwzXZZW = 0xE8, kSwzXYXY = 0x44;

DWORD Num(DWORD r) { return r & 0x7FF; }
DWORD Type(DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); }
DWORD Swz(DWORD r) { return (r >> 16) & 0xFF; }
DWORD WMask(DWORD r) { return (r >> 16) & 0xF; }
DWORD Reg(DWORD type, DWORD num) { return 0x80000000u | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | (num & 0x7FF); }
DWORD Dst(DWORD type, DWORD num, DWORD mask = 0xF, bool sat = false) { return Reg(type, num) | (mask << 16) | (sat ? 0x00100000u : 0u); }
DWORD Src(DWORD type, DWORD num, DWORD swz = kSwzXYZW) { return Reg(type, num) | (swz << 16); }
DWORD Op(DWORD op, DWORD len) { return op | (len << 24); }
DWORD F(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

struct Ins {
    size_t at; // opcode token index; operands at at+1 .. at+len
    DWORD op;
    size_t len;
};

std::vector<Ins> Parse(const std::vector<DWORD>& t) {
    std::vector<Ins> v;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFF) return v;
        if ((tok & 0xFFFF) == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (tok >> 24) & 0xF;
        if (i + len >= t.size()) return {};
        v.push_back({i, tok & 0xFFFF, len});
        i += 1 + len;
    }
    return {};
}

struct Usage {
    int maxTemp = -1, maxSampler = -1, maxConst = -1;
    size_t afterLastSamplerDcl = 0;
};

Usage Scan(const std::vector<DWORD>& t, const std::vector<Ins>& ins) {
    Usage u;
    for (const Ins& x : ins) {
        if (x.op == kDcl) {
            const DWORD r = t[x.at + 2];
            if (Type(r) == kSampler) {
                u.maxSampler = std::max(u.maxSampler, static_cast<int>(Num(r)));
                u.afterLastSamplerDcl = x.at + 1 + x.len;
            }
            continue;
        }
        if (x.op == kDef || x.op == kDefI || x.op == kDefB) {
            if (x.op == kDef) u.maxConst = std::max(u.maxConst, static_cast<int>(Num(t[x.at + 1])));
            continue;
        }
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u)) continue;
            if (Type(r) == kTemp) u.maxTemp = std::max(u.maxTemp, static_cast<int>(Num(r)));
            else if (Type(r) == kConst) u.maxConst = std::max(u.maxConst, static_cast<int>(Num(r)));
        }
    }
    return u;
}

struct Edit {
    size_t at;
    std::vector<DWORD> tok;
};

void Apply(std::vector<DWORD>& t, std::vector<Edit> edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at > b.at; });
    for (const Edit& e : edits) t.insert(t.begin() + e.at, e.tok.begin(), e.tok.end());
}

bool IsReg(DWORD r, DWORD type, DWORD num) { return (r & 0x80000000u) && Type(r) == type && Num(r) == num; }
size_t End(const Ins& x) { return x.at + 1 + x.len; }

} // namespace

namespace ShaderPatches {

bool IsRoadVs(const std::vector<DWORD>& t, DWORD& mapConst) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc1 = -1;
    bool c8 = false, c10 = false;
    for (const Ins& x : ins) {
        if (x.op == kDcl) {
            const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
            if (Type(r) == kOutput && use == 5 && idx == 1) {
                if (WMask(r) != 0x3) return false; // terrain (full) and lot (.xyz) vertex shaders stop here
                tc1 = static_cast<int>(Num(r));
            }
            continue;
        }
        if (x.op == 0x09 && x.len == 3) { // dp4 rW, rA, cK: world matrix rows
            if (IsReg(t[x.at + 3], kConst, 8)) c8 = true;
            if (IsReg(t[x.at + 3], kConst, 10)) c10 = true;
        }
    }
    if (tc1 < 0 || !c8 || !c10) return false;
    int found = 0;
    for (const Ins& x : ins) {
        // mad oT1.xy, rA.xzzw, cM, cM.zwzw
        if (x.op != kMad || !IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc1)) || WMask(t[x.at + 1]) != 0x3) continue;
        if (Type(t[x.at + 2]) != kTemp || Swz(t[x.at + 2]) != 0xE8 || Type(t[x.at + 3]) != kConst || Swz(t[x.at + 3]) != kSwzXYZW ||
            !IsReg(t[x.at + 4], kConst, Num(t[x.at + 3])) || Swz(t[x.at + 4]) != kSwzZWZW)
            return false;
        mapConst = Num(t[x.at + 3]);
        found++;
    }
    return found == 1;
}

bool PatchRoad(std::vector<DWORD>& t, RoadPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    // light map fetch: "texld rX, v1, sL" whose result is next used by the lamp scale "mul rY.xyz, rX, cN.x" (winter: right
    // after it with rY = rX and c4.x; summer: a few instructions later, rY != rX, c3.x), before rX is written again.
    int light = -1;
    for (size_t i = 0; i < ins.size() && light < 0; i++) {
        const Ins& x = ins[i];
        if (x.op != kTexld || Type(t[x.at + 1]) != kTemp || !IsReg(t[x.at + 2], kInput, 1) || Type(t[x.at + 3]) != kSampler) continue;
        const DWORD X = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < ins.size() && j <= i + 8; j++) {
            const Ins& m = ins[j];
            if (m.op == kMul && Type(t[m.at + 1]) == kTemp && (WMask(t[m.at + 1]) & 0x7) == 0x7 && IsReg(t[m.at + 2], kTemp, X) &&
                Swz(t[m.at + 2]) == kSwzXYZW && Type(t[m.at + 3]) == kConst && Swz(t[m.at + 3]) == kSwzX) {
                light = static_cast<int>(i);
                break;
            }
            bool uses = false; // any other use of rX first: not the light map
            for (size_t k = 2; k <= m.len; k++)
                if (IsReg(t[m.at + k], kTemp, X)) uses = true;
            if (uses || (m.len >= 1 && IsReg(t[m.at + 1], kTemp, X) && (WMask(t[m.at + 1]) & 0x7))) break; // rX.xyz overwritten (rX.w is fine)
        }
    }
    if (light < 0 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl) return false;
    const Ins& L = ins[light];
    const DWORD X = Num(t[L.at + 1]);
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1);
    const DWORD T0 = static_cast<DWORD>(u.maxTemp + 1);
    out.lightSampler = Num(t[L.at + 3]);
    out.extraSampler = E;
    out.sidewalkConst = -1;

    std::vector<Edit> edits;
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}});
    edits.push_back({End(L), {Op(kTexld, 3), Dst(kTemp, T0), Src(kInput, 1), Src(kSampler, E),
                              Op(kMax, 3), Dst(kTemp, X, 0x7), Src(kTemp, X), Src(kTemp, T0)}});

    // Sidewalk under snow: "mul r1.w, rA.x, rA.y" (brightness of the road texture rA) ... "lrp r1.xyz, v2.w, r0, r3" (snow).
    int albedo = -1, blend = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (albedo < 0 && x.op == kMul && IsReg(t[x.at + 1], kTemp, 1) && WMask(t[x.at + 1]) == 0x8 && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzX && Type(t[x.at + 3]) == kTemp && Num(t[x.at + 3]) == Num(t[x.at + 2]) && Swz(t[x.at + 3]) == kSwzY)
            albedo = static_cast<int>(i);
        if (blend < 0 && x.op == kLrp && IsReg(t[x.at + 1], kTemp, 1) && WMask(t[x.at + 1]) == 0x7 && IsReg(t[x.at + 2], kInput, 2) && Swz(t[x.at + 2]) == kSwzW)
            blend = static_cast<int>(i);
    }
    if (albedo >= 0 && blend > albedo && u.maxConst + 3 < 224) {
        const DWORD A = Num(t[ins[albedo].at + 2]);
        const DWORD T1 = T0 + 1, T2 = T0 + 2;
        const DWORD cS = static_cast<DWORD>(u.maxConst + 1), cL = cS + 1, cK = cS + 2;
        out.sidewalkConst = static_cast<int>(cS);
        edits.push_back({1, {Op(kDef, 5), Dst(kConst, cL), F(0.3f), F(0.59f), F(0.11f), F(0.0f),
                             Op(kDef, 5), Dst(kConst, cK), F(4.0f), F(-1.0f), F(0.0f), F(0.0f)}});
        edits.push_back({ins[albedo].at, {Op(kMov, 2), Dst(kTemp, T1), Src(kTemp, A)}});
        edits.push_back({End(ins[blend]), {Op(kDp3, 3), Dst(kTemp, T1, 0x8), Src(kTemp, T1), Src(kConst, cL),
                                           Op(kMad, 4), Dst(kTemp, T1, 0x8, true), Src(kTemp, T1, kSwzW), Src(kConst, cK, kSwzX), Src(kConst, cK, kSwzY),
                                           Op(kMul, 3), Dst(kTemp, T1, 0x8), Src(kTemp, T1, kSwzW), Src(kConst, cS, kSwzX),
                                           Op(kLrp, 4), Dst(kTemp, T2, 0x7), Src(kTemp, T1, kSwzW), Src(kTemp, T1), Src(kTemp, 1),
                                           Op(kMov, 2), Dst(kTemp, 1, 0x7), Src(kTemp, T2)}});
    }
    Apply(t, std::move(edits));
    return true;
}

bool PatchFloor(std::vector<DWORD>& t, FloorPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    size_t v0Dcl = 0;
    int fetch = -1, scale = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kDcl && IsReg(t[x.at + 2], kInput, 0) && (t[x.at + 1] & 0x1F) == 5 /* texcoord */ && ((t[x.at + 1] >> 16) & 0xF) == 0) v0Dcl = x.at + 2;
        if (fetch < 0 && x.op == kTexld && Type(t[x.at + 1]) == kTemp && IsReg(t[x.at + 2], kInput, 2) && IsReg(t[x.at + 3], kSampler, 2)) fetch = static_cast<int>(i);
        if (fetch >= 0 && scale < 0 && static_cast<int>(i) > fetch && x.op == kMul && Type(t[x.at + 1]) == kTemp && WMask(t[x.at + 1]) == 0x7 &&
            Type(t[x.at + 2]) == kTemp && Swz(t[x.at + 2]) == kSwzW && IsReg(t[x.at + 3], kTemp, Num(t[ins[fetch].at + 1])))
            scale = static_cast<int>(i);
    }
    if (!v0Dcl || fetch < 0 || scale < 0 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 1 >= 224) return false;
    const DWORD B = Num(t[ins[scale].at + 1]);
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1);
    const DWORD T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1);
    out.atlasSampler = E;
    out.atlasConst = cA;
    t[v0Dcl] = (t[v0Dcl] & ~0x000F0000u) | 0x000F0000u; // v0.xy -> v0 (zw = world xz)
    std::vector<Edit> edits;
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}});
    edits.push_back({End(ins[scale]), {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, 0, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                                       Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                                       Op(kMax, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, T)}});
    Apply(t, std::move(edits));
    return true;
}

// Winter floor tile vertex shaders: TEXCOORD0.zw = world xz ("mov o1.zw, rW.xyxz", rW.x / rW.z from dp4 with c8 / c10).
// kFloorVsBytecode is one; the curved pool edge (LightProbe-m66, VS_29991640) is another, with a pool mask in TEXCOORD7.
bool IsFloorVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc0 = -1;
    for (const Ins& x : ins)
        if (x.op == kDcl && Type(t[x.at + 2]) == kOutput && (t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == 0 && WMask(t[x.at + 2]) == 0xF)
            tc0 = static_cast<int>(Num(t[x.at + 2]));
    if (tc0 < 0) return false;
    int found = 0;
    for (const Ins& x : ins) {
        if (x.op != kMov || !IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc0)) || WMask(t[x.at + 1]) != 0xC || Type(t[x.at + 2]) != kTemp ||
            Swz(t[x.at + 2]) != kSwzXYXZ)
            continue;
        const DWORD w = Num(t[x.at + 2]);
        bool dx = false, dz = false;
        for (const Ins& y : ins) {
            if (y.op != kDp4 || !IsReg(t[y.at + 1], kTemp, w)) continue;
            if (WMask(t[y.at + 1]) == 0x1 && IsReg(t[y.at + 3], kConst, 8)) dx = true;
            if (WMask(t[y.at + 1]) == 0x4 && IsReg(t[y.at + 3], kConst, 10)) dz = true;
        }
        if (dx && dz) found++;
    }
    return found == 1;
}

// Snow lying on lot floor tiles (the big snow mesh around the pool, LightProbe-m69: VS_2FA6DE10 / PS_2FA6D640). The VS
// writes TEXCOORD7.xy = world xz * 0.5 ("mul oN.xy, rW.xzzw, cH.x", rW.x / rW.z from dp4 with c8 / c10) that the PS
// never reads; the PS lights with the room map only: "texld rL, vK, sM" then "mul rB.xyz, rS.s, rL".
bool IsSnowFloorVs(const std::vector<DWORD>& t, int& texcoord) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    // output register -> texcoord index, for TEXCOORD0 (full) and TEXCOORD7 (.xy)
    std::vector<int> tcOf(12, -1);
    for (const Ins& x : ins) {
        if (x.op != kDcl || Type(t[x.at + 2]) != kOutput || (t[x.at + 1] & 0x1F) != 5 || Num(t[x.at + 2]) >= 12) continue;
        const DWORD idx = (t[x.at + 1] >> 16) & 0xF, m = WMask(t[x.at + 2]);
        if ((idx == 7 && m == 0x3) || (idx == 0 && m == 0xF)) tcOf[Num(t[x.at + 2])] = static_cast<int>(idx);
    }
    // Terrain, lot and water vertex shaders share the TEXCOORD0.zw shape but also compute a terrain-map uv,
    // "mad oN.xy, rX, cK, cK.zwzw" (review 25/09: 5 captured terrain/water VS have it, the snow floors do not).
    for (const Ins& x : ins)
        if (x.op == kMad && Type(t[x.at + 1]) == kOutput && WMask(t[x.at + 1]) == 0x3 && Type(t[x.at + 2]) == kTemp && Type(t[x.at + 3]) == kConst &&
            Swz(t[x.at + 3]) == kSwzXYZW && IsReg(t[x.at + 4], kConst, Num(t[x.at + 3])) && Swz(t[x.at + 4]) == kSwzZWZW)
            return false;
    int found = 0;
    for (const Ins& x : ins) {
        // "mul oT7.xy, rW.xzzw, cH.s" (m69) or "mul oT0.zw, rW.xyxz, cH.s" (m71), cH.s a shader-defined 0.5 (replicate)
        if (x.op != kMul || Type(t[x.at + 1]) != kOutput || Num(t[x.at + 1]) >= 12 || Type(t[x.at + 2]) != kTemp || Type(t[x.at + 3]) != kConst) continue;
        const int tc = tcOf[Num(t[x.at + 1])];
        const DWORD m = WMask(t[x.at + 1]), s = Swz(t[x.at + 2]);
        if (!((tc == 7 && m == 0x3 && s == kSwzXZZW) || (tc == 0 && m == 0xC && s == kSwzXYXZ))) continue;
        const DWORD hs = Swz(t[x.at + 3]);
        if (hs != 0x00 && hs != 0x55 && hs != 0xAA && hs != 0xFF) continue; // the 0.5 must be one replicated component
        float half = 0;
        bool isHalf = false;
        for (const Ins& d : ins)
            if (d.op == kDef && IsReg(t[d.at + 1], kConst, Num(t[x.at + 3]))) {
                memcpy(&half, &t[d.at + 2 + (Swz(t[x.at + 3]) & 3)], 4);
                isHalf = half == 0.5f;
            }
        const DWORD w = Num(t[x.at + 2]);
        bool dx = false, dz = false;
        for (const Ins& y : ins) {
            if (y.op != kDp4 || !IsReg(t[y.at + 1], kTemp, w)) continue;
            if (WMask(t[y.at + 1]) == 0x1 && IsReg(t[y.at + 3], kConst, 8)) dx = true;
            if (WMask(t[y.at + 1]) == 0x4 && IsReg(t[y.at + 3], kConst, 10)) dz = true;
        }
        if (isHalf && dx && dz) {
            texcoord = tc;
            found++;
        }
    }
    return found == 1;
}

bool PatchSnowFloor(std::vector<DWORD>& t, int texcoord, FloorPatch& out) {
    if (texcoord != 0 && texcoord != 7) return false;
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int maxIn = -1, tcIn = -1;
    size_t afterLastInDcl = 0, tcDcl = 0;
    for (const Ins& x : ins) {
        if ((x.op >= 0x19 && x.op <= 0x1E) || (x.op >= 0x26 && x.op <= 0x2D)) return false; // flow control
        if (x.op != kDcl || Type(t[x.at + 2]) != kInput) continue;
        if ((t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == static_cast<DWORD>(texcoord)) {
            if (texcoord == 7) return false; // TEXCOORD7 already read: not the shape we know
            tcIn = static_cast<int>(Num(t[x.at + 2]));
            tcDcl = x.at + 2;
        }
        maxIn = std::max(maxIn, static_cast<int>(Num(t[x.at + 2])));
        afterLastInDcl = End(x);
    }
    // the single "texld rL, vK, sM" whose next reader is "mul rB.xyz, rS.s, rL" (the room map scaled by the bump factor)
    int scale = -1, found = 0;
    DWORD mapSampler = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kTexld || Type(t[x.at + 1]) != kTemp || Type(t[x.at + 2]) != kInput || Type(t[x.at + 3]) != kSampler) continue;
        const DWORD L = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < ins.size(); j++) {
            const Ins& y = ins[j];
            bool reads = false;
            for (size_t k = 2; k <= y.len; k++) reads |= IsReg(t[y.at + k], kTemp, L);
            if (reads) {
                const DWORD s = Type(t[y.at + 2]) == kTemp ? Swz(t[y.at + 2]) : 0x1B;
                if (y.op == kMul && Type(t[y.at + 1]) == kTemp && WMask(t[y.at + 1]) == 0x7 && Type(t[y.at + 2]) == kTemp && (s == 0x00 || s == 0x55 || s == 0xAA || s == 0xFF) &&
                    IsReg(t[y.at + 3], kTemp, L) && Swz(t[y.at + 3]) == kSwzXYZW) {
                    scale = static_cast<int>(j);
                    mapSampler = Num(t[x.at + 3]);
                    found++;
                }
                break;
            }
            if (IsReg(t[y.at + 1], kTemp, L) && (WMask(t[y.at + 1]) & 0x7)) break; // rgb overwritten (a .w write, as in m69, is fine)
        }
    }
    if (found != 1 || maxIn < 0 || maxIn >= 9 || !afterLastInDcl || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 1 >= 224 ||
        u.maxTemp + 1 >= 32)
        return false;
    const DWORD B = Num(t[ins[scale].at + 1]), E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.mapSampler = mapSampler;
    std::vector<Edit> edits;
    DWORD V, swz;
    if (texcoord == 7) { // m69: a new input, world xz / 2 in .xy
        V = static_cast<DWORD>(maxIn + 1);
        swz = kSwzXYXY;
        edits.push_back({afterLastInDcl, {Op(kDcl, 2), 0x80070005u /* texcoord7 */, Dst(kInput, V, 0x3)}});
    } else { // m71: TEXCOORD0.zw, world xz / 2; widen the declaration if the shader reads only .xy
        if (tcIn < 0) return false;
        V = static_cast<DWORD>(tcIn);
        swz = kSwzZWZW;
        t[tcDcl] |= 0x000F0000u;
    }
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}});
    edits.push_back({End(ins[scale]), {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, swz), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                                       Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                                       Op(kMax, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, T)}});
    Apply(t, std::move(edits));
    return true;
}

// Surfaces lit only by a baked light map (summer outdoor floors, ExteriorFloors technique; census 25/09): the single
// "texld rL, vK, sM" (2D sampler, input coordinate) whose next rgb reader is "mad rX.xyz, rL, cK.x, rY", before any
// flow control. Inserts max(rL, atlas) there, the atlas read at TEXCOORD7.xy = world xz (PatchObjectLampVs exports it).
bool PatchBakedAtlasPs(std::vector<DWORD>& t, int texcoord, FloorPatch& out) {
    if (texcoord < 0 || texcoord > 15) return false;
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto all = Parse(t);
    if (all.empty()) return false;
    const Usage u = Scan(t, all);
    size_t firstFlow = all.size();
    for (size_t i = 0; i < all.size(); i++)
        if ((all[i].op >= 0x19 && all[i].op <= 0x1E) || (all[i].op >= 0x26 && all[i].op <= 0x2D)) {
            firstFlow = i;
            break;
        }
    int maxIn = -1;
    size_t afterLastInDcl = 0;
    std::vector<int> is2d(16, 0);
    for (const Ins& x : all) {
        if (x.op != kDcl) continue;
        const DWORD r = t[x.at + 2];
        if (Type(r) == kSampler && Num(r) < 16) is2d[Num(r)] = ((t[x.at + 1] >> 27) & 0xF) == 2;
        if (Type(r) != kInput) continue;
        if ((t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == static_cast<DWORD>(texcoord)) return false; // that TEXCOORD already read
        maxIn = std::max(maxIn, static_cast<int>(Num(r)));
        afterLastInDcl = End(x);
    }
    if (maxIn < 0 || maxIn >= 9 || !afterLastInDcl) return false;
    int mad = -1, found = 0;
    DWORD L = 0, K = 0;
    for (size_t i = 0; i < firstFlow; i++) {
        const Ins& x = all[i];
        if (x.op != kTexld || Type(t[x.at + 1]) != kTemp || Type(t[x.at + 2]) != kInput || Type(t[x.at + 3]) != kSampler || !is2d[Num(t[x.at + 3]) & 15]) continue;
        const DWORD A = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < firstFlow; j++) {
            const Ins& y = all[j];
            bool reads = false;
            for (size_t k = 2; k <= y.len; k++) reads |= IsReg(t[y.at + k], kTemp, A) && Swz(t[y.at + k]) != kSwzW;
            if (reads) {
                if (y.op == kMad && Type(t[y.at + 1]) == kTemp && WMask(t[y.at + 1]) == 0x7 && IsReg(t[y.at + 2], kTemp, A) && Swz(t[y.at + 2]) == kSwzXYZW &&
                    Type(t[y.at + 3]) == kConst && Swz(t[y.at + 3]) == kSwzX && Type(t[y.at + 4]) == kTemp) {
                    mad = static_cast<int>(j);
                    L = A;
                    K = Num(t[y.at + 3]);
                    found++;
                }
                break;
            }
            if (IsReg(t[y.at + 1], kTemp, A) && (WMask(t[y.at + 1]) & 0x7)) break;
        }
    }
    if (found != 1) return false;
    int kUses = 0; // the scale constant is read nowhere else (not a shared constant)
    for (const Ins& x : all) {
        if (x.op == kDef || x.op == kDcl) continue;
        for (size_t k = 1; k <= x.len; k++) kUses += IsReg(t[x.at + k], kConst, K) ? 1 : 0;
    }
    if (kUses != 1 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 1 >= 224 || u.maxTemp + 1 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1), cA = static_cast<DWORD>(u.maxConst + 1);
    const DWORD V = static_cast<DWORD>(maxIn + 1);
    out.atlasSampler = E;
    out.atlasConst = cA;
    Apply(t, {{afterLastInDcl, {Op(kDcl, 2), 0x80000005u | (static_cast<DWORD>(texcoord) << 16) /* texcoordN */, Dst(kInput, V, 0x3)}},
              {u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {all[mad].at, {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                             Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                             Op(kMax, 3), Dst(kTemp, L, 0x7), Src(kTemp, L), Src(kTemp, T)}}});
    return true;
}

bool PatchLeafShadow(std::vector<DWORD>& t, DWORD& nightConst) {
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFF0000u) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    for (const Ins& x : ins) {
        // lrp rD.w, tN.x, cK.y, rS.w: the shadow fade is t5.x in the bushes of 24/09, t6.x in the winter bush of
        // LightProbe-m63 (VS_32779B38 / PS_3277A240, one more texcoord)
        // lrp rD.w, tN.x, cK.s, rS.w: cK.y at runtime in the bushes of 24/09; a shader-defined 1 in any component in the
        // summer bush of LightProbe-m78 (PS_32DDB220: "lrp r1.w, t6.x, c3.z, r2.w", def c3 = 0.5, 0.25, 1, 0)
        if (x.op != kLrp || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x8 || Type(t[x.at + 2]) != kTexture || Swz(t[x.at + 2]) != kSwzX ||
            Type(t[x.at + 3]) != kConst || Type(t[x.at + 4]) != kTemp)
            continue;
        const DWORD D = Num(t[x.at + 1]), K = Num(t[x.at + 3]), ks = Swz(t[x.at + 3]);
        if (ks != 0x00 && ks != 0x55 && ks != 0xAA && ks != 0xFF) continue;
        bool isDef = false, one = false; // the "no shadow" end of the lerp must be 1
        for (const Ins& d : ins)
            if (d.op == kDef && IsReg(t[d.at + 1], kConst, K)) {
                float v;
                memcpy(&v, &t[d.at + 2 + (ks & 3)], 4);
                isDef = true;
                one = v == 1.0f;
            }
        if (isDef ? !one : ks != kSwzY) continue;
        const DWORD T = static_cast<DWORD>(u.maxTemp + 1);
        const DWORD cN = static_cast<DWORD>(u.maxConst + 1);
        if (T >= 12 || cN >= 32) return false; // ps_2_0 limits
        nightConst = cN;
        // rD.w += cN.x * (cK.y - rD.w), as two instructions: ps_2_0 allows only one constant register per instruction
        Apply(t, {{End(x), {Op(kAdd, 3), Dst(kTemp, T, 0x8), Src(kConst, K, ks), Src(kTemp, D, kSwzW) | 0x01000000u /* negate */,
                            Op(kMad, 4), Dst(kTemp, D, 0x8), Src(kTemp, T, kSwzW), Src(kConst, cN, kSwzX), Src(kTemp, D, kSwzW)}}});
        return true;
    }
    return false;
}

bool PatchFoliageVs(std::vector<DWORD>& t) {
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFE0000u) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    bool lampArray = false;
    for (const Ins& x : ins)
        for (size_t k = 1; k <= x.len; k++)
            if (IsReg(t[x.at + k], kConst, 27) && (t[x.at + k] & 0x2000)) lampArray = true; // c27[a0.z]: per-instance lamp directions
    if (!lampArray) return false;
    // "def cK, ...": the component `swz` (a replicate swizzle) of a shader-defined constant, if there is one
    auto defined = [&](DWORD k, DWORD swz, float& v) {
        for (const Ins& d : ins)
            if (d.op == kDef && IsReg(t[d.at + 1], kConst, k)) {
                memcpy(&v, &t[d.at + 2 + (swz & 3)], 4);
                return true;
            }
        return false;
    };
    for (const Ins& x : ins) {
        // max r0, r0, cK.w (a runtime zero), or max r0, r0, cK.s with cK.s a shader-defined 0 (the winter bush of
        // LightProbe-m63, VS_32779B38: "max r0, r0, c131.x", def c131 = 0, ...)
        if (x.op != kMax || !IsReg(t[x.at + 1], kTemp, 0) || WMask(t[x.at + 1]) != 0xF || !IsReg(t[x.at + 2], kTemp, 0) || Swz(t[x.at + 2]) != kSwzXYZW ||
            Type(t[x.at + 3]) != kConst || (t[x.at + 3] & 0x2000))
            continue;
        const DWORD K = Num(t[x.at + 3]), swz = Swz(t[x.at + 3]);
        float v = 1.0f;
        const bool isDef = defined(K, swz, v);
        if (isDef ? !((swz == 0x00 || swz == 0x55 || swz == 0xAA || swz == 0xFF) && v == 0.0f) : swz != kSwzW) continue;
        constexpr DWORD cW = 255;
        t[x.at + 1] = Dst(kTemp, 0, 0x1); // the sun keeps the plain clamp
        Apply(t, {{1, {Op(kDef, 5), Dst(kConst, cW), F(1.0f / 1.5f), F(0.5f / 1.5f), F(0.0f), F(0.0f)}},
                  {End(x), {Op(kMad, 4), Dst(kTemp, 0, 0xE), Src(kTemp, 0), Src(kConst, cW, kSwzX), Src(kConst, cW, kSwzY),
                            Op(kMax, 3), Dst(kTemp, 0, 0xE), Src(kTemp, 0), Src(kConst, K, swz)}}});
        return true;
    }
    return false;
}

// Instanced lot structures (fence rails/posts, railings, stairs). Found by the fence analysis (NOTAS-ILUMINACAO.md):
// their VS reads only the rig's "vertex light" arrays (VertexLightDirections/Colors, rig+0x90/+0xD0), which the game
// fills only with overflow lights, and the whole group shares one rig placed at its centre. So fences are lit per pixel
// from the ground light atlas instead.
bool IsInstancedStructureVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    bool pos1 = false, pos2 = false;
    int tc1Out = -1, col0Out = -1;
    for (const Ins& x : ins) {
        if (x.op == kMova) return false;
        if (x.op != kDef && x.op != kDefI && x.op != kDefB && x.op != kDcl)
            for (size_t k = 1; k <= x.len; k++)
                if ((t[x.at + k] & 0x80000000u) && (t[x.at + k] & 0x2000)) return false; // relative addressing
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 0 && idx == 1) pos1 = true; // POSITION1: instance translation
        if (Type(r) == kInput && use == 0 && idx == 2) pos2 = true; // POSITION2: instance rotation
        if (Type(r) == kOutput && use == 5 && idx == 1) tc1Out = static_cast<int>(Num(r));
        if (Type(r) == kOutput && use == 10 && idx == 0) col0Out = static_cast<int>(Num(r));
    }
    if (!pos1 || !pos2 || tc1Out < 0 || col0Out < 0) return false;
    bool world = false, lamps = false;
    for (const Ins& x : ins) {
        // mov oT.zw, rW.xyxz: world xz into TEXCOORD1.zw
        if (x.op == kMov && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc1Out)) && WMask(t[x.at + 1]) == 0xC && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzXYXZ)
            world = true;
        // mad oC.xyz, rX.w, c11, rY: the last of the 4 vertex lights into COLOR0
        if (x.op == kMad && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(col0Out)) && IsReg(t[x.at + 3], kConst, 11)) lamps = true;
    }
    return world && lamps;
}

bool PatchInstancedLamps(std::vector<DWORD>& t, InstancedPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int colorReg = -1, tc1Reg = -1;
    size_t tc1Dcl = 0;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) != kInput) continue;
        if (use == 10 && idx == 0 && (WMask(r) & 0x7) == 0x7) colorReg = static_cast<int>(Num(r)); // COLOR0: vertex lights
        if (use == 5 && idx == 1) {                                                                 // TEXCOORD1: zw = world xz
            tc1Reg = static_cast<int>(Num(r));
            tc1Dcl = x.at + 2;
        }
    }
    if (colorReg < 0 || tc1Reg < 0) return false;
    // the single "add rD.xyz, rS, vC" (lamps added to sun x shadow + ambient)
    int add = -1, found = 0, slot = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kAdd || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x7) continue;
        for (int s = 2; s <= 3; s++)
            if (IsReg(t[x.at + s], kInput, static_cast<DWORD>(colorReg)) && Swz(t[x.at + s]) == kSwzXYZW && Type(t[x.at + (s == 2 ? 3 : 2)]) == kTemp) {
                add = static_cast<int>(i);
                slot = s;
                found++;
            }
    }
    if (found != 1 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 1 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1;
    const DWORD C = static_cast<DWORD>(colorReg), V = static_cast<DWORD>(tc1Reg);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    t[tc1Dcl] = (t[tc1Dcl] & ~0x000F0000u) | 0x000F0000u; // vT.xy -> vT (zw = world xz)
    const Ins& A = ins[add];
    t[A.at + slot] = Src(kTemp, T); // add rD.xyz, rS, vC  ->  add rD.xyz, rS, rT
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {A.at, {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                      Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                      Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kConst, cB, kSwzX),
                      Op(kMax, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kInput, C)}}});
    return true;
}

bool IsSnowCoverVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc3Out = -1, tc1In = -1;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kOutput && use == 5 && idx == 3 && WMask(r) == 0x3) tc3Out = static_cast<int>(Num(r)); // TEXCOORD3.xy
        if (Type(r) == kInput && use == 5 && idx == 1) tc1In = static_cast<int>(Num(r));                      // TEXCOORD1
    }
    if (tc3Out < 0 || tc1In < 0) return false;
    bool morph = false;
    int world = -1, found = 0;
    for (const Ins& x : ins) {
        // slt rX, cK, vT1.x: the snow cover's position morph flags
        if (x.op == kSlt && IsReg(t[x.at + 3], kInput, static_cast<DWORD>(tc1In)) && Swz(t[x.at + 3]) == kSwzX) morph = true;
        // mov oT3.xy, rW.xzzw: world xz
        if (x.op == kMov && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc3Out)) && WMask(t[x.at + 1]) == 0x3 && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzXZZW) {
            world = static_cast<int>(Num(t[x.at + 2]));
            found++;
        }
    }
    if (!morph || found != 1) return false;
    bool dx = false, dz = false; // rW.x = dp4(pos, c8), rW.z = dp4(pos, c10): world matrix rows
    for (const Ins& x : ins) {
        if (x.op != kDp4 || !IsReg(t[x.at + 1], kTemp, static_cast<DWORD>(world))) continue;
        if (WMask(t[x.at + 1]) == 0x1 && IsReg(t[x.at + 3], kConst, 8)) dx = true;
        if (WMask(t[x.at + 1]) == 0x4 && IsReg(t[x.at + 3], kConst, 10)) dz = true;
    }
    return dx && dz;
}

bool PatchSnowCover(std::vector<DWORD>& t, SnowCoverPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int tc3 = -1;
    for (const Ins& x : ins) {
        if ((x.op >= 0x19 && x.op <= 0x1E) || (x.op >= 0x26 && x.op <= 0x2D)) return false; // flow control (call..label, rep..breakc): leave it alone
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 5 && idx == 3 && (WMask(r) & 0x3) == 0x3) tc3 = static_cast<int>(Num(r)); // TEXCOORD3: world xz
    }
    if (tc3 < 0) return false;
    // the single "mul oC0.xyz, rP, rQ"
    int mul = -1, found = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kMul && IsReg(t[x.at + 1], kColorOut, 0) && WMask(t[x.at + 1]) == 0x7 && Type(t[x.at + 2]) == kTemp && Type(t[x.at + 3]) == kTemp) {
            mul = static_cast<int>(i);
            found++;
        }
    }
    if (found != 1) return false;
    auto lastWriter = [&](DWORD reg) {
        for (int i = mul - 1; i >= 0; i--) {
            const Ins& x = ins[i];
            if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB || x.op == kTexkill || x.len == 0) continue;
            if (IsReg(t[x.at + 1], kTemp, reg)) return i;
        }
        return -1;
    };
    const DWORD P = Num(t[ins[mul].at + 2]), Q = Num(t[ins[mul].at + 3]);
    const int wP = lastWriter(P), wQ = lastWriter(Q);
    if (wP < 0 || wQ < 0) return false;
    // the snow texture comes from a texld, the light from "mad rL.xyz, rX.w, c0, rY" (moon x shadow + sky)
    int wL = -1;
    DWORD L = 0;
    if (ins[wQ].op == kTexld) {
        wL = wP;
        L = P;
    } else if (ins[wP].op == kTexld) {
        wL = wQ;
        L = Q;
    } else
        return false;
    const Ins& M = ins[wL];
    if (M.op != kMad || WMask(t[M.at + 1]) != 0x7 || Type(t[M.at + 2]) != kTemp || Swz(t[M.at + 2]) != kSwzW || !IsReg(t[M.at + 3], kConst, 0)) return false;
    if (u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 1 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, V = static_cast<DWORD>(tc3);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {ins[mul].at, {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                             Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                             Op(kMad, 4), Dst(kTemp, L, 0x7), Src(kTemp, T), Src(kConst, cB, kSwzX), Src(kTemp, L)}}});
    return true;
}

bool IsFlowControl(DWORD op) { return (op >= 0x19 && op <= 0x1E) || (op >= 0x26 && op <= 0x2D); }

bool PatchObjectLampVs(std::vector<DWORD>& t, bool needColor0, int* texcoordOut, int* worldConstOut, int* vertexLightOut) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int maxOut = -1, col0 = -1;
    size_t afterLastOutDcl = 0;
    uint32_t tcUsed = 0, posIn = 0, nrmIn = 0; // TEXCOORD indices already written; POSITIONn / NORMALn inputs
    for (const Ins& x : ins) {
        // Skinned objects (an animated door: mova + c[a0] bones inside "if b0", LightProbe-m57) are fine as long as the
        // world position below is computed outside any branch; loops, subroutines and labels are not.
        if (IsFlowControl(x.op) && x.op != 0x28 && x.op != 0x29 && x.op != 0x2A && x.op != 0x2B && x.op != 0x1C) return false;
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput) {
            if (use == 0) posIn |= 1u << idx;
            if (use == 3) nrmIn |= 1u << idx;
            continue;
        }
        if (Type(r) != kOutput) continue;
        maxOut = std::max(maxOut, static_cast<int>(Num(r)));
        afterLastOutDcl = End(x);
        if (use == 10 && idx == 0) col0 = static_cast<int>(Num(r));
        if (use == 5) tcUsed |= 1u << idx;
    }
    // POSITIONn together with NORMALn: morph targets (176 Phong VS blend POSITION1..3 / NORMAL1..3 with c26), fine;
    // POSITION1/2 alone would be instancing.
    if (posIn & ~nrmIn & ~1u) return false;
    // Objects: TEXCOORD8, which none of the 2139 Counters/Phong shaders use (Counters has the sink cut-out uv in
    // TEXCOORD7, Phong a projective coordinate); PatchObjectLampPs reads 8. Floors (texcoordOut) take the first free one
    // from 7 (census 25/09: many ExteriorFloors vertex shaders already write TEXCOORD7).
    int tcIdx = 8;
    if (texcoordOut) {
        tcIdx = 7;
        while (tcIdx < 16 && (tcUsed & (1u << tcIdx))) tcIdx++;
    }
    if ((needColor0 && col0 < 0) || tcIdx >= 16 || (tcUsed & (1u << tcIdx)) || maxOut < 0 || maxOut >= 11 || !afterLastOutDcl) return false;
    auto hasDst = [&](const Ins& x) {
        return x.len > 0 && x.op != kDcl && x.op != kDef && x.op != kDefI && x.op != kDefB && x.op != kTexkill && !IsFlowControl(x.op) && (t[x.at + 1] & 0x80000000u);
    };
    // the world position: dp4 rW.x / .y / .z, rP, cK / cK+1 / cK+2 outside branches. A triple stays open until something
    // else writes rP, or a component of rW it already has (split triples, Phong_VS_3875: dp4 r1.x ... 16 instructions ...
    // dp4 r1.z, dp4 r1.y).
    struct Tri {
        DWORD w, p;
        int k[3] = {-1, -1, -1};
        DWORD filled = 0;
        bool open = true;
        size_t first = SIZE_MAX, last = 0, lastEnd = 0;
    };
    std::vector<Tri> tris;
    int depth = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == 0x28 || x.op == 0x29) depth++; // if, ifc
        if (x.op == 0x2B) depth--;                 // endif
        if (!hasDst(x)) continue;
        const DWORD d = t[x.at + 1], m = WMask(d);
        const bool temp = Type(d) == kTemp;
        int c = -1;
        if (depth == 0 && x.op == kDp4 && x.len == 3 && temp && Type(t[x.at + 2]) == kTemp && Swz(t[x.at + 2]) == kSwzXYZW && Type(t[x.at + 3]) == kConst &&
            !(t[x.at + 3] & 0x2000) /* c[a0 + n]: not a fixed matrix */)
            c = m == 0x1 ? 0 : m == 0x2 ? 1 : m == 0x4 ? 2 : -1;
        size_t tri = SIZE_MAX;
        if (c >= 0)
            for (size_t q = 0; q < tris.size(); q++)
                if (tris[q].open && tris[q].w == Num(d) && tris[q].p == Num(t[x.at + 2]) && tris[q].k[c] < 0) tri = q;
        if (temp)
            for (size_t q = 0; q < tris.size(); q++)
                if (q != tri && tris[q].open && (Num(d) == tris[q].p || (Num(d) == tris[q].w && (m & tris[q].filled)))) tris[q].open = false;
        if (c < 0) continue;
        if (tri == SIZE_MAX) {
            tris.push_back({Num(d), Num(t[x.at + 2])});
            tri = tris.size() - 1;
        }
        Tri& q = tris[tri];
        q.k[c] = static_cast<int>(Num(t[x.at + 3]));
        q.filled |= m;
        q.first = std::min(q.first, i);
        if (i >= q.last) {
            q.last = i;
            q.lastEnd = End(x);
        }
        if (q.w == q.p) q.open = false; // "dp4 r0.x, r0, cK": the source is gone
    }
    std::vector<const Tri*> full;
    for (const Tri& q : tris)
        if (q.k[0] >= 0 && q.k[1] == q.k[0] + 1 && q.k[2] == q.k[0] + 2) full.push_back(&q);
    const Tri* world = full.size() == 1 ? full[0] : nullptr;
    if (full.size() > 1) {
        // Several consecutive-constant triples (census 25/09, Phong VS_E3A718D3: world c19..c21, then a view triple
        // c12..c14 computed from the world position): keep the one whose source comes straight from the POSITION input.
        int posReg = -1;
        for (const Ins& x : ins)
            if (x.op == kDcl && Type(t[x.at + 2]) == kInput && (t[x.at + 1] & 0x1F) == 0 && ((t[x.at + 1] >> 16) & 0xF) == 0) posReg = static_cast<int>(Num(t[x.at + 2]));
        const Tri* fromPos = nullptr;
        int nFromPos = 0;
        for (const Tri* q : full) {
            if (posReg < 0) break;
            // last writer of the source register before the triple
            for (size_t i = q->first; i-- > 0;) {
                const Ins& x = ins[i];
                if (!hasDst(x) || !IsReg(t[x.at + 1], kTemp, q->p)) continue;
                bool readsPos = false;
                for (size_t k = 2; k <= x.len; k++) readsPos |= IsReg(t[x.at + k], kInput, static_cast<DWORD>(posReg));
                if (readsPos) {
                    fromPos = q;
                    nFromPos++;
                }
                break;
            }
        }
        if (nFromPos == 1) world = fromPos;
        else {
            // Otherwise the root: the one triple whose source does not depend on another triple's result (morphs and
            // skinning build the position first; Phong_VS_3810: world c195..c197, then a view-projection c188..c190 from
            // it). Forward taint from each other triple's result up to this triple's first dp4.
            int nRoot = 0;
            for (const Tri* q : full) {
                bool dep = false;
                for (const Tri* s : full) {
                    if (s == q || s->last >= q->first || s->w >= 32) continue;
                    uint32_t taint = 1u << s->w;
                    for (size_t i = s->last + 1; i < q->first; i++) {
                        const Ins& x = ins[i];
                        if (!hasDst(x) || Type(t[x.at + 1]) != kTemp || Num(t[x.at + 1]) >= 32) continue;
                        bool src = false;
                        for (size_t k = 2; k <= x.len; k++) {
                            const DWORD r = t[x.at + k];
                            if ((r & 0x80000000u) && Type(r) == kTemp && Num(r) < 32 && ((taint >> Num(r)) & 1)) src = true;
                        }
                        const DWORD r = Num(t[x.at + 1]);
                        if (src) taint |= 1u << r;
                        else if (WMask(t[x.at + 1]) == 0xF) taint &= ~(1u << r);
                    }
                    if (q->p < 32 && ((taint >> q->p) & 1)) dep = true;
                }
                if (!dep) {
                    world = q;
                    nRoot++;
                }
            }
            if (nRoot != 1) return false;
        }
    }
    if (!world) return false;
    // The rig's 4 vertex lights (every Counters/Phong vs_3_0, 25/09): "dp3_sat rS.s, cD, rN" (either order) then
    // "mul/mad rX.xyz, rS.s, cD+4[, rP]" for 4 consecutive directions cD0..cD0+3; the colours cD0+4..cD0+7 are c4, c8, c184
    // or c188. The last step writes COLOR0.xyz or feeds its last writer (Phong adds "mad oC0.xyz, vNormal.w, cAmbient, r").
    int vlK = -1;
    if (col0 >= 0) {
        auto isReplicate = [](DWORD swz) { return swz == 0x00 || swz == 0x55 || swz == 0xAA || swz == 0xFF; };
        int dirOf[32][4];
        for (auto& a : dirOf)
            for (int& v : a) v = -1;
        std::vector<char> pair(256, 0);
        std::vector<std::pair<size_t, int>> steps;
        for (size_t i = 0; i < ins.size(); i++) {
            const Ins& x = ins[i];
            if (!hasDst(x)) continue;
            const DWORD d = t[x.at + 1];
            if (((x.op == kMul && x.len == 3) || (x.op == kMad && x.len == 4)) && Type(t[x.at + 2]) == kTemp && Num(t[x.at + 2]) < 32 && isReplicate(Swz(t[x.at + 2])) &&
                Type(t[x.at + 3]) == kConst && !(t[x.at + 3] & 0x2000)) {
                const int dir = dirOf[Num(t[x.at + 2])][Swz(t[x.at + 2]) & 3], k = static_cast<int>(Num(t[x.at + 3]));
                if (dir >= 0 && dir < 252 && k == dir + 4) {
                    pair[dir] = 1;
                    steps.push_back({i, k});
                }
            }
            if (Type(d) != kTemp || Num(d) >= 32) continue;
            for (int c = 0; c < 4; c++)
                if ((WMask(d) >> c) & 1) dirOf[Num(d)][c] = -1;
            if (x.op == kDp3 && x.len == 3 && (d & 0x00100000u)) {
                const DWORD a = t[x.at + 2], b = t[x.at + 3], cr = Type(a) == kConst ? a : Type(b) == kConst ? b : 0;
                if (cr && !(cr & 0x2000))
                    for (int c = 0; c < 4; c++)
                        if ((WMask(d) >> c) & 1) dirOf[Num(d)][c] = static_cast<int>(Num(cr));
            }
        }
        int base = -1, groups = 0;
        for (int dd = 0; dd + 3 < 256; dd++)
            if (pair[dd] && pair[dd + 1] && pair[dd + 2] && pair[dd + 3]) {
                base = dd;
                groups++;
            }
        if (groups == 1) {
            const int K = base + 4;
            size_t li = SIZE_MAX, lw = SIZE_MAX;
            for (const auto& [i, k] : steps)
                if (k == K + 3) li = i;
            for (size_t i = 0; i < ins.size(); i++)
                if (hasDst(ins[i]) && IsReg(t[ins[i].at + 1], kOutput, static_cast<DWORD>(col0)) && (WMask(t[ins[i].at + 1]) & 0x7)) lw = i;
            bool feeds = li != SIZE_MAX && lw != SIZE_MAX && li == lw;
            if (li != SIZE_MAX && lw != SIZE_MAX && lw > li && Type(t[ins[li].at + 1]) == kTemp)
                for (size_t k = 2; k <= ins[lw].len; k++) feeds |= IsReg(t[ins[lw].at + k], kTemp, Num(t[ins[li].at + 1]));
            if (feeds) vlK = K;
        }
    }
    const DWORD N = static_cast<DWORD>(maxOut + 1), W = world->w;
    const size_t at = world->lastEnd;
    if (texcoordOut) *texcoordOut = tcIdx;
    if (worldConstOut) *worldConstOut = world->k[0];
    if (vertexLightOut) *vertexLightOut = vlK;
    // .xy = world xz (ground atlas uv), .z = world y (per-pixel lamps on objects)
    Apply(t, {{afterLastOutDcl, {Op(kDcl, 2), 0x80000005u | (static_cast<DWORD>(tcIdx) << 16) /* texcoordN */, Dst(kOutput, N, 0x7)}},
              {at, {Op(kMov, 2), Dst(kOutput, N, 0x7), Src(kTemp, W, 0xD8 /* xzyw */)}}});
    return true;
}

bool PatchObjectLampPs(std::vector<DWORD>& t, ObjectLampPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto all = Parse(t);
    if (all.empty()) return false;
    const Usage u = Scan(t, all);
    int colorReg = -1, maxIn = -1;
    size_t afterLastInDcl = 0;
    bool tc8 = false;
    std::vector<int> cubeSampler(16, 0);
    // Everything this patch reads and inserts must come before the first flow-control instruction (census 25/09: the
    // Phong PS_C249A5C0 ends with an if/else block after the lighting).
    size_t firstFlow = all.size();
    for (size_t i = 0; i < all.size(); i++)
        if (IsFlowControl(all[i].op)) {
            firstFlow = i;
            break;
        }
    const std::vector<Ins> ins(all.begin(), all.begin() + firstFlow);
    for (const Ins& x : all) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kSampler && Num(r) < 16) cubeSampler[Num(r)] = ((t[x.at + 1] >> 27) & 0xF) == 3;
        if (Type(r) != kInput) continue;
        maxIn = std::max(maxIn, static_cast<int>(Num(r)));
        afterLastInDcl = End(x);
        if (use == 10 && idx == 0 && (WMask(r) & 0x7) == 0x7) colorReg = static_cast<int>(Num(r)); // COLOR0: vertex lights
        if (use == 5 && idx == 8) tc8 = true;
    }
    if (tc8 || maxIn < 0 || maxIn >= 9 || !afterLastInDcl) return false;
    // last instruction before `before` that writes any of `mask`'s components of temp `reg`
    auto lastWriter = [&](DWORD reg, int before, DWORD mask) {
        for (int i = before - 1; i >= 0; i--) {
            const Ins& x = ins[i];
            if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB || x.op == kTexkill || x.len == 0) continue;
            if (IsReg(t[x.at + 1], kTemp, reg) && (WMask(t[x.at + 1]) & mask)) return i;
        }
        return -1;
    };
    auto isReplicate = [](DWORD swz) { return swz == 0x00 || swz == 0x55 || swz == 0xAA || swz == 0xFF; };
    // the last rig-lamp step: "mad rD.xyz, rS.s, c7, rP" (the third lamp; rP = rD or another accumulator)
    auto chainEnd = [&](int i) {
        const Ins& L = ins[i];
        return L.op == kMad && L.len == 4 && Type(t[L.at + 1]) == kTemp && WMask(t[L.at + 1]) == 0x7 && Type(t[L.at + 2]) == kTemp && isReplicate(Swz(t[L.at + 2])) &&
               IsReg(t[L.at + 3], kConst, 7) && Type(t[L.at + 4]) == kTemp;
    };
    // "mad rA.xyz, rCube, cK.s, rD": the sky (a cube texld at the normal) plus the diffuse lamp chain rD, whose last step is
    // chainEnd. Returns the cube texld's index.
    auto isCubeTexld = [&](int i) {
        const Ins& K = ins[i];
        return K.op == kTexld && Type(t[K.at + 2]) == kTemp && Swz(t[K.at + 2]) == kSwzXYZW && Type(t[K.at + 3]) == kSampler && cubeSampler[Num(t[K.at + 3]) & 15];
    };
    auto skyAt = [&](int i) -> int {
        const Ins& S = ins[i];
        if (S.op != kMad || Type(t[S.at + 1]) != kTemp || WMask(t[S.at + 1]) != 0x7 || Type(t[S.at + 2]) != kTemp || Type(t[S.at + 3]) != kConst ||
            !isReplicate(Swz(t[S.at + 3])) || Type(t[S.at + 4]) != kTemp)
            return -1;
        const int wCube = lastWriter(Num(t[S.at + 2]), i, 0x7), wD = lastWriter(Num(t[S.at + 4]), i, 0x7);
        if (wCube < 0 || wD < 0 || !isCubeTexld(wCube) || !chainEnd(wD)) return -1;
        return wCube;
    };
    // Three shapes (Counters/Phong analysis 25/09, scratchpad counter_sh):
    //  A: rig lamps + sky cube (+ "add rX.xyz, rA, vC"): insert at the sky mad; the normal is the cube coordinate.
    //  B: the 4-light chain c0..c3 / c4..c7 with no cube and no COLOR0 (8 Counters): insert right after the chain end.
    //  C: no lamps in the pixel shader, light = max(vC, per-object light map, sky) (32 Phong): the lamps go into vC.
    enum { kA, kB, kC } shape = kA;
    int gi = -1, ref = -1, anchor = -1;
    DWORD Nrm = 0, D = 0;
    if (colorReg >= 0) {
        // with vertex lights: the single "add rX.xyz, rA, vC" (the destination may be another register: generic objects
        // write r4 from r0); the sky mad is rA's last writer
        const DWORD C = static_cast<DWORD>(colorReg);
        int add = -1, found = 0;
        DWORD A = 0;
        for (size_t i = 0; i < ins.size(); i++) {
            const Ins& x = ins[i];
            if (x.op != kAdd || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x7) continue;
            const bool c2 = IsReg(t[x.at + 2], kInput, C) && Swz(t[x.at + 2]) == kSwzXYZW && Type(t[x.at + 3]) == kTemp;
            const bool c3 = IsReg(t[x.at + 3], kInput, C) && Swz(t[x.at + 3]) == kSwzXYZW && Type(t[x.at + 2]) == kTemp;
            if (c2 || c3) {
                add = static_cast<int>(i);
                A = Num(t[x.at + (c2 ? 3 : 2)]);
                found++;
            }
        }
        if (found == 1) {
            anchor = lastWriter(A, add, 0x7);
            if (anchor >= 0) ref = skyAt(anchor);
        }
        if (found != 1 || ref < 0) {
            // shape C: the single "max rX.xyz, vC, rY" (either order) and the single cube texld (its coordinate: the normal)
            if (found != 0) return false;
            int mx = -1, nMax = 0, cube = -1, nCube = 0;
            for (size_t i = 0; i < ins.size(); i++) {
                const Ins& x = ins[i];
                if (isCubeTexld(static_cast<int>(i))) {
                    cube = static_cast<int>(i);
                    nCube++;
                }
                if (x.op != kMax || x.len != 3 || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x7) continue;
                for (int k = 2; k <= 3; k++)
                    if (IsReg(t[x.at + k], kInput, C) && Swz(t[x.at + k]) == kSwzXYZW && !(t[x.at + k] & 0x0F000000u) && Type(t[x.at + 5 - k]) == kTemp) {
                        mx = static_cast<int>(i);
                        nMax++;
                    }
            }
            if (nMax != 1 || nCube != 1) return false;
            shape = kC;
            anchor = mx;
            // the lamp code goes at the cube texld when it comes first (Phong_PS_3113 overwrites the normal before the max)
            gi = cube < mx ? cube : mx;
            ref = cube;
            Nrm = Num(t[ins[cube].at + 2]);
        } else {
            gi = ref;
            Nrm = Num(t[ins[ref].at + 2]);
            D = Num(t[ins[anchor].at + 4]);
        }
    } else {
        // no vertex lights (objects lit by the rig only, e.g. PS_298DF5B8): the single sky mad of that shape
        int found = 0;
        for (size_t i = 0; i < ins.size(); i++)
            if (skyAt(static_cast<int>(i)) >= 0) {
                anchor = static_cast<int>(i);
                found++;
            }
        if (found == 1) {
            ref = gi = skyAt(anchor);
            Nrm = Num(t[ins[ref].at + 2]);
            D = Num(t[ins[anchor].at + 4]);
        } else {
            // shape B (Counters_PS_440): no sky; the single chain end, whose result is the light
            if (found != 0) return false;
            int nEnd = 0;
            for (size_t i = 0; i < ins.size(); i++)
                if (chainEnd(static_cast<int>(i))) {
                    anchor = static_cast<int>(i);
                    nEnd++;
                }
            if (nEnd != 1) return false;
            // the normal: the one register lit by both the first lamp (c1) and the sun / first light (c0, c9 or c13)
            int nN = 0;
            for (int r = 0; r < 32; r++) {
                int r1 = -1;
                bool sunR = false;
                for (int i = 0; i < anchor; i++) {
                    const Ins& x = ins[i];
                    if (x.op != kDp3 || x.len != 3) continue;
                    for (int k = 2; k <= 3; k++) {
                        if (!IsReg(t[x.at + k], kTemp, static_cast<DWORD>(r)) || Swz(t[x.at + k]) != kSwzXYZW) continue;
                        const DWORD o = t[x.at + 5 - k];
                        if (IsReg(o, kConst, 1)) r1 = i;
                        if (IsReg(o, kConst, 0) || IsReg(o, kConst, 9) || IsReg(o, kConst, 13)) sunR = true;
                    }
                }
                if (r1 >= 0 && sunR) {
                    Nrm = static_cast<DWORD>(r);
                    ref = r1;
                    nN++;
                }
            }
            if (nN != 1) return false;
            shape = kB;
            gi = anchor;
            D = Num(t[ins[anchor].at + 1]);
        }
    }
    // the normal must still hold the same value where the new code reads it
    if (lastWriter(Nrm, gi, 0x7) != lastWriter(Nrm, ref, 0x7)) return false;
    if (shape != kC) {
        // that normal also lights the first lamp ("dp3 ..., rN, c1") and the sun: c9 in the 3-lamp shaders, c0 in the
        // 4-light ones (PS_2D041628: c0..c3 directions, c4..c7 colours), c13 in Phong (census 25/09)
        bool sun = false, lamp1 = false;
        for (size_t i = 0; i < ins.size() && static_cast<int>(i) < std::max(gi, anchor); i++) {
            const Ins& x = ins[i];
            if (x.op != kDp3) continue;
            for (int k = 2; k <= 3; k++) {
                if (!IsReg(t[x.at + k], kTemp, Nrm)) continue;
                const DWORD o = t[x.at + 5 - k];
                if (IsReg(o, kConst, 9) || IsReg(o, kConst, 0) || IsReg(o, kConst, 13)) sun = true;
                if (IsReg(o, kConst, 1)) lamp1 = true;
            }
        }
        if (!sun || !lamp1) return false;
    }
    if (u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 4 + 2 * kObjectPixelLamps >= 224 || u.maxTemp + 5 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1), Fr = T + 1;
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, cH = cA + 2, V = static_cast<DWORD>(maxIn + 1);
    const DWORD cS = cA + 3, cL = cA + 4; // per-pixel lamps: parameters, then kObjectPixelLamps x (pos + 1/R^2, colour)
    const DWORD A = T + 2, B = T + 3, Q = T + 4;
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    out.lampParamConst = cS;
    out.lampConst = cL;
    out.rigLamps = shape != kC;
    std::vector<DWORD> ground = {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                                 Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                                 Op(kMad, 4), Dst(kTemp, Fr, 0x8), Src(kTemp, Nrm, kSwzY), Src(kConst, cH, kSwzX), Src(kConst, cH, kSwzY),
                                 Op(kMul, 3), Dst(kTemp, Fr, 0x8), Src(kTemp, Fr, kSwzW), Src(kConst, cB, kSwzX),
                                 Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, Fr, kSwzW)};
    if (shape == kA && colorReg >= 0) // the vertex lights are added after the max: take them out of the ground light first
        ground.insert(ground.end(), {Op(kAdd, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kInput, static_cast<DWORD>(colorReg)) | 0x01000000u /* -vC */});
    // Per-pixel lamps (the friend's "Counters" request, 25/09): each piece of a modular object gets its own rig
    // evaluated at the piece's centre (3 lamps in c5..c7 here, 4 more as vertex lights), so neighbouring pieces differ.
    // Q = the same world lamps for every pixel: sum of colour * sat(N.l) * sat(1 - d^2/R^2)^2 at the pixel's world
    // position (TEXCOORD8.xzy). The draw zeroes the rig (PS c5..c7 and the VS vertex-light colours) while cS.y > 0.
    // cS = (unused, lamp strength, unused, 1e-4).
    constexpr DWORD kNeg = 0x01000000u;
    const DWORD pw = Src(kInput, V, 0xD8 /* xzyw: world x, y, z */);
    for (DWORD k = 0; k < kObjectPixelLamps; k++) {
        const DWORD cp = cL + 2 * k, cc = cp + 1;
        ground.insert(ground.end(), {Op(kAdd, 3), Dst(kTemp, A, 0x7), Src(kConst, cp), pw | kNeg,          // l = lamp - pixel
                                     Op(kDp3, 3), Dst(kTemp, A, 0x8), Src(kTemp, A), Src(kTemp, A),         // d^2
                                     Op(kMax, 3), Dst(kTemp, A, 0x8), Src(kTemp, A, kSwzW), Src(kConst, cS, kSwzW),
                                     Op(0x07 /* rsq */, 2), Dst(kTemp, B, 0x8), Src(kTemp, A, kSwzW),
                                     Op(kMul, 3), Dst(kTemp, A, 0x7), Src(kTemp, A), Src(kTemp, B, kSwzW),  // normalize
                                     Op(kDp3, 3), Dst(kTemp, B, 0x1, true), Src(kTemp, Nrm), Src(kTemp, A),  // sat(N.l)
                                     Op(kMad, 4), Dst(kTemp, B, 0x2, true), Src(kTemp, A, kSwzW), Src(kConst, cp, kSwzW) | kNeg, Src(kConst, cH, 0xAA /* 1 */),
                                     Op(kMul, 3), Dst(kTemp, B, 0x2), Src(kTemp, B, kSwzY), Src(kTemp, B, kSwzY),
                                     Op(kMul, 3), Dst(kTemp, B, 0x1), Src(kTemp, B, kSwzX), Src(kTemp, B, kSwzY)});
        if (k == 0) ground.insert(ground.end(), {Op(kMul, 3), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, B, kSwzX)});
        else ground.insert(ground.end(), {Op(kMad, 4), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, B, kSwzX), Src(kTemp, Q)});
    }
    std::vector<Edit> edits = {{1, {Op(kDef, 5), Dst(kConst, cH), F(0.5f), F(0.5f), F(1.0f), F(0.0f)}},
                               {afterLastInDcl, {Op(kDcl, 2), 0x80080005u /* texcoord8 */, Dst(kInput, V, 0x7)}},
                               {u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}}};
    if (shape == kC) {
        // before "max rX.xyz, vC, rY": rA = max(vC + Q * cS.y, ground), and the max reads rA instead of vC
        const Ins& M = ins[anchor];
        const std::vector<DWORD> atMax = {Op(kMad, 4), Dst(kTemp, A, 0x7), Src(kTemp, Q), Src(kConst, cS, kSwzY), Src(kInput, static_cast<DWORD>(colorReg)),
                                          Op(kMax, 3), Dst(kTemp, A, 0x7), Src(kTemp, A), Src(kTemp, T)};
        for (size_t k = 2; k <= 3; k++)
            if (IsReg(t[M.at + k], kInput, static_cast<DWORD>(colorReg))) t[M.at + k] = Src(kTemp, A);
        if (gi == anchor) {
            ground.insert(ground.end(), atMax.begin(), atMax.end());
            edits.push_back({M.at, ground});
        } else {
            edits.push_back({ins[gi].at, ground});
            edits.push_back({M.at, atMax});
        }
        Apply(t, edits);
        return true;
    }
    // A: at the sky mad (before it), B: right after the chain end: rD = max(rD + Q * cS.y, ground)
    const std::vector<DWORD> atEnd = {Op(kMad, 4), Dst(kTemp, D, 0x7), Src(kTemp, Q), Src(kConst, cS, kSwzY), Src(kTemp, D),
                                      Op(kMax, 3), Dst(kTemp, D, 0x7), Src(kTemp, D), Src(kTemp, T)};
    if (shape == kB) {
        // the chain end may overwrite the normal (Counters_PS_440: r1 = normal): the ground and lamp code goes before it,
        // only the final mad / max after it
        edits.push_back({ins[gi].at, ground});
        edits.push_back({End(ins[anchor]), atEnd});
    } else {
        edits.push_back({ins[gi].at, ground});
        edits.push_back({ins[anchor].at, atEnd});
    }
    Apply(t, edits);
    return true;
}

bool IsSnowReliefVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc4Out = -1;
    bool tc2In = false;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kOutput && use == 5 && idx == 4 && (WMask(r) & 0xC) == 0xC) tc4Out = static_cast<int>(Num(r)); // TEXCOORD4 with zw
        if (Type(r) == kInput && use == 5 && idx == 2) tc2In = true; // TEXCOORD2: the snow's base position (roofs, same family, have none)
    }
    if (tc4Out < 0 || !tc2In) return false;
    int world = -1, found = 0;
    DWORD half = 0;
    for (const Ins& x : ins) {
        // mul oT4.zw, rW.xyxz, cD.x
        if (x.op == kMul && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc4Out)) && WMask(t[x.at + 1]) == 0xC && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzXYXZ && Type(t[x.at + 3]) == kConst && Swz(t[x.at + 3]) == kSwzX) {
            world = static_cast<int>(Num(t[x.at + 2]));
            half = Num(t[x.at + 3]);
            found++;
        }
    }
    if (found != 1) return false;
    bool halfOk = false, dx = false, dz = false;
    for (const Ins& x : ins) {
        if (x.op == kDef && IsReg(t[x.at + 1], kConst, half)) {
            float v;
            memcpy(&v, &t[x.at + 2], 4);
            halfOk = v == 0.5f;
        }
        if (x.op != kDp4 || !IsReg(t[x.at + 1], kTemp, static_cast<DWORD>(world))) continue;
        if (WMask(t[x.at + 1]) == 0x1 && IsReg(t[x.at + 3], kConst, 8)) dx = true;
        if (WMask(t[x.at + 1]) == 0x4 && IsReg(t[x.at + 3], kConst, 10)) dz = true;
    }
    return halfOk && dx && dz;
}

bool PatchSnowRelief(std::vector<DWORD>& t, SnowCoverPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int tc4 = -1;
    bool cube0 = false;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 5 && idx == 4 && (WMask(r) & 0xC) == 0xC) tc4 = static_cast<int>(Num(r)); // TEXCOORD4: zw = world xz / 2
        if (IsReg(r, kSampler, 0) && ((t[x.at + 1] >> 27) & 0xF) == 3) cube0 = true;                            // dcl_cube s0
    }
    if (tc4 < 0 || !cube0) return false;
    // "texld rC, rN, s0" and the single "mad rL.xyz, rC, cK.x, rS" outside any loop
    int mad = -1, found = 0, depth = 0;
    int cubeReg = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == 0x26 || x.op == 0x1B) depth++; // rep, loop
        if (x.op == 0x27 || x.op == 0x1D) depth--; // endrep, endloop
        if (x.op == kTexld && Type(t[x.at + 1]) == kTemp && IsReg(t[x.at + 3], kSampler, 0)) {
            cubeReg = static_cast<int>(Num(t[x.at + 1]));
            continue;
        }
        if (cubeReg >= 0 && depth == 0 && x.op == kMad && Type(t[x.at + 1]) == kTemp && WMask(t[x.at + 1]) == 0x7 &&
            IsReg(t[x.at + 2], kTemp, static_cast<DWORD>(cubeReg)) && Type(t[x.at + 3]) == kConst && Swz(t[x.at + 3]) == kSwzX && Type(t[x.at + 4]) == kTemp) {
            mad = static_cast<int>(i);
            found++;
        }
    }
    if (found != 1 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 1 >= 32) return false;
    const DWORD L = Num(t[ins[mad].at + 1]);
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, V = static_cast<DWORD>(tc4);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {End(ins[mad]), {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                               Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                               Op(kMad, 4), Dst(kTemp, L, 0x7), Src(kTemp, T), Src(kConst, cB, kSwzX), Src(kTemp, L)}}});
    return true;
}

} // namespace ShaderPatches
