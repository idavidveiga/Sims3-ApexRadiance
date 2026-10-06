#pragma once
// Structural recognition of the game's weather variants of a known pixel shader (Apex Radiance, Night Lighting).
//
// The rain variants captured on 2026-10-05 are the dry shader with (a) its def'd constants moved to other registers and
// (b) a different tail after the colour is computed: instead of `mul oC0.xyz, r0, cK` they do `mul rA.xyz, r0, cK` and
// post-process rA with constants the body never reads (wetness c5.x, c6.x), then write oC0. Exact size + hash ids
// (shader_ids.h) miss every such variant, so the lot and world terrain fell out of step when it rained.
//
// A shader matches a family when:
//   - its body, canonicalised (comments dropped, def'd float constants renumbered by order of definition), has the
//     dry body's token count and FNV-1a hash: every instruction up to the first write of a colour output, identical;
//   - its tail starts with the dry final instruction's operation and sources, written to a temp instead of oC0;
//   - the tail has no texture reads, declarations, flow control or relative addressing, reads only temps, def'd constants
//     and constants the body never reads, and writes oC0 (only oC0).
// Only identifiers (counts and hashes) are kept, never the game's bytecode.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ShaderStructure {

struct StructId {
    uint32_t bodyTokens = 0; // canonical tokens before the first colour-output write (version token included)
    uint32_t bodyHash = 0;   // FNV-1a over those canonical tokens
    uint32_t finalHash = 0;  // FNV-1a over the final instruction's opcode token and canonical source tokens
};

enum class Tail : uint8_t { Dry, Wet, Other };
struct Match {
    Tail tail = Tail::Other;
    uint32_t wetScale = 0, wetMix = 0; // Wet: oC0 = a + cScale.x * (cMix.x * a - a); the registers of cScale and cMix (rain: c5, c6)
};

namespace detail {

constexpr uint32_t kOpDcl = 0x1F, kOpDef = 0x51, kOpDefI = 0x30, kOpDefB = 0x2F, kOpComment = 0xFFFE, kOpEnd = 0xFFFF;
constexpr uint32_t kOpMov = 0x01, kOpMad = 0x04, kOpMul = 0x05;
constexpr uint32_t kRegTemp = 0, kRegConst = 2, kRegColorOut = 8;
constexpr uint32_t kCanonBase = 0x780; // canonical numbers of def'd constants (11-bit field)

inline uint32_t RegType(uint32_t r) { return ((r >> 28) & 7) | ((r >> 8) & 0x18); }
inline uint32_t RegNum(uint32_t r) { return r & 0x7FF; }
inline uint32_t Fnv(uint32_t h, uint32_t d) { return (h ^ d) * 16777619u; }
inline bool IsTexOp(uint32_t op) { return op == 0x41 || op == 0x42 || (op >= 0x5D && op <= 0x5F) || op == 0x40; } // texkill, tex(ld), texldd/texldl..., texcoord
inline bool IsFlowOp(uint32_t op) { return (op >= 0x19 && op <= 0x1E) || (op >= 0x26 && op <= 0x2E) || op == 0x60; } // call..label, rep..mova, breakp

struct Ins {
    uint32_t at = 0, len = 0, op = 0; // token index of the opcode, operand count
};

// Instructions of a ps_2_x / ps_3_0 token stream (comments skipped). False when malformed.
inline bool Parse(const uint32_t* t, size_t n, std::vector<Ins>& out) {
    if (n < 2 || (t[0] & 0xFFFF0000u) != 0xFFFF0000u) return false;
    for (size_t i = 1; i < n;) {
        const uint32_t op = t[i] & 0xFFFF;
        if (op == kOpEnd) return true;
        if (op == kOpComment) {
            i += 1 + ((t[i] >> 16) & 0x7FFF);
            continue;
        }
        const uint32_t len = (t[i] >> 24) & 0x0F;
        if (i + 1 + len > n) return false;
        out.push_back({static_cast<uint32_t>(i), len, op});
        i += 1 + len;
    }
    return false;
}

struct Canon {
    uint32_t map[256]; // def'd float constant number -> canonical number (0 = not def'd)
    Canon() { for (uint32_t& m : map) m = 0; }
    uint32_t Reg(uint32_t r) const {
        if (RegType(r) != kRegConst || RegNum(r) >= 256 || !map[RegNum(r)]) return r;
        return (r & ~0x7FFu) | map[RegNum(r)];
    }
};

// Canonical tokens of one instruction: def'd constants renumbered, a dcl's usage token kept as is, def values kept.
inline void CanonIns(const uint32_t* t, const Ins& x, const Canon& c, std::vector<uint32_t>& out) {
    out.push_back(t[x.at]);
    for (uint32_t k = 1; k <= x.len; k++) {
        const uint32_t tok = t[x.at + k];
        const bool reg = x.op == kOpDcl ? k == 2 : (x.op == kOpDef || x.op == kOpDefI || x.op == kOpDefB) ? k == 1 : true;
        out.push_back(reg ? c.Reg(tok) : tok);
    }
}

inline bool WritesOutput(const uint32_t* t, const Ins& x) {
    return x.len >= 1 && x.op != kOpDcl && x.op != kOpDef && x.op != kOpDefI && x.op != kOpDefB && RegType(t[x.at + 1]) == kRegColorOut;
}

struct Split {
    std::vector<Ins> ins;
    Canon canon;
    size_t first = 0; // index in ins of the first colour-output write
    uint32_t bodyTokens = 0, bodyHash = 0;
};

inline bool SplitShader(const uint32_t* t, size_t n, Split& s) {
    if (!Parse(t, n, s.ins)) return false;
    uint32_t next = 0;
    for (const Ins& x : s.ins)
        if (x.op == kOpDef && x.len == 5 && RegType(t[x.at + 1]) == kRegConst && RegNum(t[x.at + 1]) < 256 && !s.canon.map[RegNum(t[x.at + 1])])
            s.canon.map[RegNum(t[x.at + 1])] = kCanonBase + next++;
    s.first = s.ins.size();
    for (size_t i = 0; i < s.ins.size(); i++)
        if (WritesOutput(t, s.ins[i])) {
            s.first = i;
            break;
        }
    if (s.first == s.ins.size()) return false;
    std::vector<uint32_t> body{t[0]};
    for (size_t i = 0; i < s.first; i++) CanonIns(t, s.ins[i], s.canon, body);
    uint32_t h = 2166136261u;
    for (uint32_t d : body) h = Fnv(h, d);
    s.bodyTokens = static_cast<uint32_t>(body.size());
    s.bodyHash = h;
    return true;
}

// Opcode token (with modifiers) and canonical source tokens of an instruction, hashed
inline uint32_t FinalHash(const uint32_t* t, const Ins& x, const Canon& c) {
    uint32_t h = Fnv(2166136261u, t[x.at] & 0x0FFFFFFFu);
    for (uint32_t k = 2; k <= x.len; k++) h = Fnv(h, c.Reg(t[x.at + k]));
    return h;
}

} // namespace detail

// The family id of a dry shader (its first colour write must be a mul): false when it cannot serve as a family
inline bool MakeId(const uint32_t* t, size_t n, StructId& out) {
    detail::Split s;
    if (!detail::SplitShader(t, n, s) || s.ins[s.first].op != detail::kOpMul) return false;
    out = {s.bodyTokens, s.bodyHash, detail::FinalHash(t, s.ins[s.first], s.canon)};
    return true;
}

// Whether the shader is the family's dry shader or a weather variant of it (see the header comment)
inline bool MatchId(const uint32_t* t, size_t n, const StructId& id, Match& m) {
    using namespace detail;
    Split s;
    if (!SplitShader(t, n, s)) return false;
    // The body ends where the dry one wrote oC0: the variant computes the same value into a temp at that point
    if (s.bodyTokens > id.bodyTokens) {
        // the variant's first output write comes later: its body is the dry body plus the tail's first instructions
        std::vector<uint32_t> body{t[0]};
        size_t i = 0;
        for (; i < s.first && body.size() < id.bodyTokens; i++) CanonIns(t, s.ins[i], s.canon, body);
        if (body.size() != id.bodyTokens) return false;
        uint32_t h = 2166136261u;
        for (uint32_t d : body) h = Fnv(h, d);
        if (h != id.bodyHash) return false;
        const Ins& f = s.ins[i]; // the dry final operation, into a temp
        if (f.op != kOpMul || f.len < 1 || RegType(t[f.at + 1]) != kRegTemp || FinalHash(t, f, s.canon) != id.finalHash) return false;
        // Constants the body reads (Apex reinterprets them; the tail must not depend on them)
        bool bodyConst[256] = {};
        for (size_t k = 0; k < i; k++) {
            const Ins& x = s.ins[k];
            if (x.op == kOpDcl || x.op == kOpDef || x.op == kOpDefI || x.op == kOpDefB) continue;
            for (uint32_t a = 2; a <= x.len; a++)
                if (RegType(t[x.at + a]) == kRegConst && RegNum(t[x.at + a]) < 256) bodyConst[RegNum(t[x.at + a])] = true;
        }
        bool wroteOut = false;
        for (size_t k = i; k < s.ins.size(); k++) {
            const Ins& x = s.ins[k];
            if (x.op == kOpDcl || x.op == kOpDef || x.op == kOpDefI || x.op == kOpDefB || IsTexOp(x.op) || IsFlowOp(x.op) || x.len < 1) return false;
            const uint32_t dt = RegType(t[x.at + 1]);
            if (dt == kRegColorOut) {
                if (RegNum(t[x.at + 1]) != 0) return false;
                wroteOut = true;
            } else if (dt != kRegTemp) return false;
            for (uint32_t a = 2; a <= x.len; a++) {
                const uint32_t r = t[x.at + a];
                if (r & 0x2000u) return false; // relative addressing
                const uint32_t rt = RegType(r);
                if (rt == kRegTemp) continue;
                if (rt != kRegConst || RegNum(r) >= 256) return false;
                if (!s.canon.map[RegNum(r)] && bodyConst[RegNum(r)]) return false; // a body constant read again by the tail
            }
        }
        if (!wroteOut) return false;
        m = {};
        m.tail = Tail::Other;
        // Known rain tail: mul a.xyz, <final>; mad b.xyz, cMix.x, a, -a; mad oC0.xyz, cScale.x, b, a; mov oC0.w, <def>
        if (s.ins.size() == i + 4) {
            const Ins &b = s.ins[i + 1], &o = s.ins[i + 2], &w = s.ins[i + 3];
            const uint32_t a = RegNum(t[f.at + 1]);
            auto src = [&](const Ins& x, uint32_t k) { return t[x.at + k]; };
            const bool shape = b.op == kOpMad && b.len == 4 && o.op == kOpMad && o.len == 4 && w.op == kOpMov && w.len == 2 &&
                               RegType(src(b, 1)) == kRegTemp && RegType(src(b, 2)) == kRegConst && ((src(b, 2) >> 16) & 0xFF) == 0x00 &&
                               RegType(src(b, 3)) == kRegTemp && RegNum(src(b, 3)) == a && RegType(src(b, 4)) == kRegTemp && RegNum(src(b, 4)) == a &&
                               ((src(b, 4) >> 24) & 0xF) == 1 && ((src(b, 3) >> 24) & 0xF) == 0 &&
                               RegType(src(o, 1)) == kRegColorOut && ((src(o, 1) >> 16) & 0xF) == 7 && RegType(src(o, 2)) == kRegConst &&
                               ((src(o, 2) >> 16) & 0xFF) == 0x00 && RegType(src(o, 3)) == kRegTemp && RegNum(src(o, 3)) == RegNum(src(b, 1)) &&
                               ((src(o, 3) >> 24) & 0xF) == 0 && RegType(src(o, 4)) == kRegTemp && RegNum(src(o, 4)) == a && ((src(o, 4) >> 24) & 0xF) == 0 &&
                               RegType(src(w, 1)) == kRegColorOut && ((src(w, 1) >> 16) & 0xF) == 8;
            if (shape) {
                m.tail = Tail::Wet;
                m.wetMix = RegNum(src(b, 2));
                m.wetScale = RegNum(src(o, 2));
            }
        }
        return true;
    }
    if (s.bodyTokens != id.bodyTokens || s.bodyHash != id.bodyHash) return false;
    if (FinalHash(t, s.ins[s.first], s.canon) != id.finalHash) return false;
    m = {};
    m.tail = Tail::Dry; // the dry shader itself, or a copy with only its def'd constants renumbered
    return true;
}

} // namespace ShaderStructure
