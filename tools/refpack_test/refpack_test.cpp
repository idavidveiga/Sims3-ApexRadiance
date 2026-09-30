// Offline check of features/refpack_codec.{h,cpp}: the fast RefPack compressor against the translation of The Sims 3's
// decompressor (TS3W.exe 0x004EB3B0) and compressor (0x004EC0A0 + 0x004EB750 / 0x004EBB90):
//   1. round trips for every size 0..300 and for random sizes up to 2 MB, nine kinds of data, all four flag modes the
//      game uses (0 and 1: 16 KB window, 2: 128 KB window, 0x10000: 16 KB window, first positions only);
//   2. a strict stream check (header as the game writes it, every opcode well formed, offsets inside the window and the
//      output, the stop opcode last, output length = the header's size);
//   3. counting runs (no destination) return the size the real run writes; a capacity one byte short fails without
//      writing past it;
//   4. compressed size and time against the game's compressor, per kind, and a sweep of the search depth.
// Console output only; writes no files. The game's decoder itself checks the streams in game (the ASI's development
// build decompresses every stream with it by default: Developer > Profiler > Performance).
//
// Build (x86 like the game; x64 also works for this file), from "x86 Native Tools Command Prompt for VS 2022" or after
//   "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat":
//   cd /d C:\Users\luiz_\Desktop\S3SS-dev\S3SSApex\tools\refpack_test
//   cl /nologo /O2 /EHsc /std:c++20 /I..\..\features refpack_test.cpp ..\..\features\refpack_codec.cpp /Fe:refpack_test.exe
// Run:
//   refpack_test.exe                 full run (a few minutes: the game's compressor is the slow part)
//   refpack_test.exe --quick         fewer buffers
//   refpack_test.exe --file C:\x.bin also a file's bytes (read only; repeat --file for more)
// Exit code 0 = every round trip and stream check passed.
//
// Part of Apex Radiance. Credits: @loinyx
#include "refpack_codec.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

using namespace RefPackCodec;

namespace {

struct Rng {
    uint64_t s;
    uint32_t Next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<uint32_t>((s * 2685821657736338717ull) >> 32);
    }
    uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

double NowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return 1000.0 * static_cast<double>(q.QuadPart) / static_cast<double>(f.QuadPart);
}

enum Kind { kRandom, kZeros, kRuns, kPeriodic, kText, kTexture, kMesh, kFarRepeats, kSparse, kKinds };
const char* const kKindName[kKinds] = {"random", "zeros", "runs", "periodic", "text", "texture RGBA", "mesh floats", "far repeats", "sparse"};

void Generate(Rng& r, Kind k, uint32_t n, std::vector<uint8_t>& v) {
    v.assign(n, 0);
    switch (k) {
    case kRandom:
        for (auto& b : v) b = static_cast<uint8_t>(r.Next());
        break;
    case kZeros:
        break;
    case kRuns:
        for (uint32_t i = 0; i < n;) {
            const uint8_t b = static_cast<uint8_t>(r.Next());
            const uint32_t len = 1 + r.Below(r.Below(4) ? 12 : 400);
            for (uint32_t j = 0; j < len && i < n; j++) v[i++] = b;
        }
        break;
    case kPeriodic: {
        const uint32_t p = 1 + r.Below(64);
        std::vector<uint8_t> pat(p);
        for (auto& b : pat) b = static_cast<uint8_t>(r.Next());
        for (uint32_t i = 0; i < n; i++) v[i] = r.Below(200) ? pat[i % p] : static_cast<uint8_t>(r.Next());
        break;
    }
    case kText: {
        static const char* const words[] = {"the", "sim", "house", "lot", "light", "night", "lamp", "garden", "kitchen", "window", "door", "Sims3", "texture",
                                            "resource", "package", "neighborhood", "career", "skill", "moodlet", "wall", "roof", "floor", "tree", "pool"};
        uint32_t i = 0;
        while (i < n) {
            const char* w = words[r.Below(static_cast<uint32_t>(sizeof words / sizeof words[0]))];
            for (const char* c = w; *c && i < n; c++) v[i++] = static_cast<uint8_t>(*c);
            if (i < n) v[i++] = r.Below(12) ? ' ' : '\n';
        }
        break;
    }
    case kTexture: { // BGRA rows: gradients with a little noise, rows sometimes repeated
        const uint32_t width = 64u << r.Below(4);
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            const uint32_t px = i / 4, x = px % width, y = px / width;
            v[i] = static_cast<uint8_t>(x * 2 + r.Below(3));
            v[i + 1] = static_cast<uint8_t>(y * 3 + r.Below(2));
            v[i + 2] = static_cast<uint8_t>((x + y) + r.Below(4));
            v[i + 3] = (x & 16) ? 255 : static_cast<uint8_t>(200 + r.Below(3));
            if (x == 0 && y > 0 && r.Below(4) == 0 && i >= width * 4) { // repeat the previous row
                for (uint32_t j = 0; j < width * 4 && i + j < n; j++) v[i + j] = v[i + j - width * 4];
                i += width * 4 - 4;
            }
        }
        break;
    }
    case kMesh: { // vertex floats of a smooth surface, then 16-bit indices
        uint32_t i = 0;
        float t = 0.0f;
        while (i + 12 <= n * 3 / 4) {
            const float p[3] = {std::sin(t) * 10.0f, std::cos(t * 0.7f) * 10.0f, t};
            std::memcpy(&v[i], p, 12);
            i += 12;
            t += 0.01f;
        }
        uint16_t idx = 0;
        while (i + 2 <= n) {
            std::memcpy(&v[i], &idx, 2);
            i += 2;
            idx = static_cast<uint16_t>(idx + (r.Below(3) ? 1 : 0));
        }
        break;
    }
    case kFarRepeats: { // random bytes with copies from 20 KB, 100 KB and 130 KB back (inside / beyond the windows)
        for (auto& b : v) b = static_cast<uint8_t>(r.Next());
        static const uint32_t dists[] = {20000, 100000, 130000, 16384, 16383, 131071, 131072};
        for (uint32_t i = 0; i < n;) {
            const uint32_t d = dists[r.Below(7)];
            const uint32_t len = 8 + r.Below(600);
            if (i >= d)
                for (uint32_t j = 0; j < len && i < n; j++, i++) v[i] = v[i - d];
            else
                i += len;
            i += r.Below(3000);
        }
        break;
    }
    case kSparse:
        for (auto& b : v)
            if (!r.Below(20)) b = static_cast<uint8_t>(r.Next());
        break;
    default:
        break;
    }
}

// Strict check of a stream: header as the game writes it for (size, flags), opcodes, window, stop opcode last
bool Validate(const std::vector<uint8_t>& s, uint32_t n, uint32_t size, uint32_t flags, std::string& why) {
    const Params p = ParamsFor(size, flags);
    if (n < 2 + p.sizeBytes + 1) return why = "too short", false;
    const uint32_t hdr = (static_cast<uint32_t>(s[0]) << 8) | s[1];
    if (hdr != p.header) return why = "header differs from the game's", false;
    uint32_t declared = 0;
    for (uint32_t i = 0; i < p.sizeBytes; i++) declared = (declared << 8) | s[2 + i];
    if (declared != size) return why = "declared size differs", false;
    uint32_t pos = 2 + p.sizeBytes, out = 0;
    for (;;) {
        if (pos >= n) return why = "no stop opcode", false;
        const uint32_t a = s[pos];
        uint32_t lit = 0, len = 0, off = 0, bytes = 1;
        if (a < 0x80) {
            if (pos + 2 > n) return why = "truncated 2-byte opcode", false;
            lit = a & 3;
            len = ((a >> 2) & 7) + 3;
            off = ((a & 0x60) << 3) + s[pos + 1] + 1;
            bytes = 2;
        } else if (a < 0xC0) {
            if (pos + 3 > n) return why = "truncated 3-byte opcode", false;
            lit = s[pos + 1] >> 6;
            len = (a & 0x3F) + 4;
            off = ((s[pos + 1] & 0x3F) << 8) + s[pos + 2] + 1;
            bytes = 3;
        } else if (a < 0xE0) {
            if (pos + 4 > n) return why = "truncated 4-byte opcode", false;
            lit = a & 3;
            off = ((a & 0x10) << 12) + (s[pos + 1] << 8) + s[pos + 2] + 1;
            len = ((a & 0x0C) << 6) + s[pos + 3] + 5;
            bytes = 4;
        } else if (a < 0xFC) {
            lit = ((a & 0x1F) + 1) * 4;
        } else {
            lit = a & 3;
            if (pos + 1 + lit != n) return why = "bytes after the stop opcode", false;
            out += lit;
            if (out != size) return why = "output length differs from the size", false;
            return true;
        }
        pos += bytes;
        if (pos + lit > n) return why = "literals run past the stream", false;
        pos += lit;
        out += lit;
        if (len) {
            if (off > out) return why = "offset before the start of the output", false;
            if (off > p.window) return why = "offset outside the window", false;
            out += len;
        }
        if (out > size) return why = "output longer than the size", false;
    }
}

struct Totals {
    uint64_t in = 0, fastOut = 0, gameOut = 0;
    double fastMs = 0, gameMs = 0;
};

Context MakeContext(std::vector<uint8_t>& mem) {
    mem.assign(ContextBytes() + 16, 0);
    Context c;
    InitContext(c, mem.data());
    return c;
}

uint64_t g_fail = 0;
int g_printed = 0;
void Fail(const char* what, Kind k, uint32_t size, uint32_t flags, const std::string& why) {
    g_fail++;
    if (g_printed++ < 20) std::printf("  FAIL %s: %s, %u bytes, flags %#x: %s\n", what, kKindName[k], size, flags, why.c_str());
}

// Fast compress + checks; returns the stream size (0 on failure)
uint32_t CheckOne(Context& ctx, const std::vector<uint8_t>& src, Kind k, uint32_t flags, const Effort& ef, double* ms) {
    const uint32_t size = static_cast<uint32_t>(src.size());
    std::vector<uint8_t> out(static_cast<size_t>(SizeBound(size)) + 64, 0xEE);
    const double t0 = NowMs();
    const uint32_t r = Compress(ctx, src.data(), size, out.data(), 0, flags, ef);
    if (ms) *ms += NowMs() - t0;
    if (r == kFailed || r > SizeBound(size)) return Fail("compress", k, size, flags, "no stream or larger than the game's bound"), 0;
    std::string why;
    if (!Validate(out, r, size, flags, why)) return Fail("stream check", k, size, flags, why), 0;
    std::vector<uint8_t> back(size + 1, 0xAB);
    const uint32_t got = Decompress(back.data(), size, out.data(), r);
    if (got != size || std::memcmp(back.data(), src.data(), size) != 0) return Fail("round trip (game's decoder)", k, size, flags, "decoded data differs"), 0;
    if (Compress(ctx, src.data(), size, nullptr, 0, flags, ef) != r) return Fail("counting run", k, size, flags, "size differs from the written stream"), 0;
    if (r > 0) { // one byte short: must fail and write nothing past the capacity
        std::vector<uint8_t> tight(static_cast<size_t>(r) + 16, 0x5A);
        if (Compress(ctx, src.data(), size, tight.data(), r - 1, flags, ef) != kFailed) return Fail("capacity", k, size, flags, "did not report a stream that does not fit"), 0;
        for (size_t i = r - 1; i < tight.size(); i++)
            if (tight[i] != 0x5A) return Fail("capacity", k, size, flags, "wrote past the capacity"), 0;
        if (Compress(ctx, src.data(), size, tight.data(), r, flags, ef) != r) return Fail("capacity", k, size, flags, "exact capacity refused"), 0;
    }
    return r;
}

// Segmented compression + checks: the stream checks of CheckOne, and the same bytes when the pieces are parsed in reverse
// order on two alternating contexts (as threads would); returns the stream size (0 on failure)
std::vector<Token> g_tokA(kMaxSegmentTokens), g_tokB;
uint32_t CheckSegmented(Context& ctx, Context& ctx2, const std::vector<uint8_t>& src, Kind k, uint32_t flags, const Effort& ef, double* ms) {
    const uint32_t size = static_cast<uint32_t>(src.size());
    std::vector<uint8_t> out(static_cast<size_t>(SizeBound(size)) + 64, 0xEE);
    const double t0 = NowMs();
    const uint32_t r = CompressSegmented(ctx, src.data(), size, out.data(), 0, flags, ef, g_tokA.data());
    if (ms) *ms += NowMs() - t0;
    if (r == kFailed || r > SizeBound(size)) return Fail("segmented compress", k, size, flags, "no stream or larger than the game's bound"), 0;
    std::string why;
    if (!Validate(out, r, size, flags, why)) return Fail("segmented stream check", k, size, flags, why), 0;
    std::vector<uint8_t> back(size + 1, 0xAB);
    const uint32_t got = Decompress(back.data(), size, out.data(), r);
    if (got != size || std::memcmp(back.data(), src.data(), size) != 0) return Fail("segmented round trip (game's decoder)", k, size, flags, "decoded data differs"), 0;
    if (CompressSegmented(ctx, src.data(), size, nullptr, 0, flags, ef, g_tokA.data()) != r) return Fail("segmented counting run", k, size, flags, "size differs"), 0;
    // pieces parsed last to first, alternating two contexts, then written in order: the same bytes
    const uint32_t segs = SegmentCount(size);
    std::vector<std::vector<Token>> toks(segs);
    std::vector<uint32_t> counts(segs);
    for (uint32_t s = segs; s-- > 0;) {
        toks[s].resize(kMaxSegmentTokens);
        counts[s] = ParseSegment((s & 1) ? ctx2 : ctx, src.data(), size, flags, s, ef, toks[s].data());
        if (counts[s] > kMaxSegmentTokens) return Fail("segmented tokens", k, size, flags, "more tokens than kMaxSegmentTokens"), 0;
    }
    std::vector<uint8_t> out2(out.size(), 0xEE);
    SegmentEncoder enc;
    enc.Begin(src.data(), size, out2.data(), 0, flags);
    for (uint32_t s = 0; s < segs; s++) enc.Add(toks[s].data(), counts[s]);
    if (enc.End() != r || std::memcmp(out.data(), out2.data(), r) != 0) return Fail("segmented order", k, size, flags, "another parse order gave other bytes"), 0;
    if (r > 0) { // one byte short: must fail and write nothing past the capacity; exact capacity works
        std::vector<uint8_t> tight(static_cast<size_t>(r) + 16, 0x5A);
        if (CompressSegmented(ctx, src.data(), size, tight.data(), r - 1, flags, ef, g_tokA.data()) != kFailed)
            return Fail("segmented capacity", k, size, flags, "did not report a stream that does not fit"), 0;
        for (size_t i = r - 1; i < tight.size(); i++)
            if (tight[i] != 0x5A) return Fail("segmented capacity", k, size, flags, "wrote past the capacity"), 0;
        if (CompressSegmented(ctx, src.data(), size, tight.data(), r, flags, ef, g_tokA.data()) != r) return Fail("segmented capacity", k, size, flags, "exact capacity refused"), 0;
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    bool quick = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--file") && i + 1 < argc) files.push_back(argv[++i]);
    }
    std::printf("RefPack test: fast compressor vs the translations of TS3W.exe's decompressor (0x004EB3B0) and compressor (0x004EC0A0)\n");
    Rng rng{0xC0FFEE1234ull};
    std::vector<uint8_t> mem;
    Context ctx = MakeContext(mem);
    const Effort ef{}; // the ASI's defaults
    const uint32_t kFlags[4] = {0, 1, 2, 0x10000};
    std::vector<uint8_t> src;

    // ---- 1. every size 0..300 ----
    {
        uint64_t n = 0;
        for (int k = 0; k < kKinds; k++)
            for (uint32_t size = 0; size <= 300; size++)
                for (uint32_t f : kFlags) {
                    Generate(rng, static_cast<Kind>(k), size, src);
                    CheckOne(ctx, src, static_cast<Kind>(k), f, ef, nullptr);
                    n++;
                }
        std::printf("Sizes 0..300: %llu streams checked, %llu failures so far\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_fail));
    }

    // ---- 2. random sizes up to 2 MB ----
    {
        const int buffers = quick ? 60 : 400;
        uint64_t bytes = 0;
        for (int i = 0; i < buffers; i++) {
            const Kind k = static_cast<Kind>(rng.Below(kKinds));
            const uint32_t size = static_cast<uint32_t>(std::exp(std::log(301.0) + (std::log(2097152.0) - std::log(301.0)) * (rng.Next() / 4294967296.0)));
            Generate(rng, k, size, src);
            CheckOne(ctx, src, k, kFlags[rng.Below(4)], ef, nullptr);
            bytes += size;
        }
        std::printf("Random sizes up to 2 MB: %d buffers (%.1f MB), %llu failures so far\n", buffers, static_cast<double>(bytes) / 1048576.0, static_cast<unsigned long long>(g_fail));
    }

    // ---- 3. size and speed against the game's compressor ----
    std::printf("\nAgainst the game's compressor (translation), per kind; flags 2 = the memory caches' 128 KB window, flags 0 = 16 KB window:\n");
    std::printf("  %-14s %5s %9s | %10s %9s | %10s %9s | %7s %7s\n", "kind", "flags", "size", "game out", "game ms", "fast out", "fast ms", "size", "speed");
    Totals all;
    // (the game's search compares every chain entry in the window: on repetitive data its time grows much faster than the
    // size, so the comparison stays at 64 KB / 256 KB)
    const std::vector<uint32_t> sizes = quick ? std::vector<uint32_t>{65536u} : std::vector<uint32_t>{65536u, 262144u};
    for (int k = 0; k < kKinds; k++)
        for (uint32_t f : {2u, 0u})
            for (uint32_t size : sizes) {
                Generate(rng, static_cast<Kind>(k), size, src);
                std::vector<uint8_t> g(static_cast<size_t>(SizeBound(size)) + 64);
                const double t0 = NowMs();
                const uint32_t gs = GameCompress(src.data(), size, g.data(), f);
                const double gms = NowMs() - t0;
                std::string why;
                if (!Validate(g, gs, size, f, why)) Fail("game translation stream check", static_cast<Kind>(k), size, f, why);
                std::vector<uint8_t> back(size + 1);
                if (Decompress(back.data(), size, g.data(), gs) != size || std::memcmp(back.data(), src.data(), size) != 0)
                    Fail("game translation round trip", static_cast<Kind>(k), size, f, "decoded data differs");
                if (GameCompress(src.data(), size, nullptr, f) != gs) Fail("game translation counting run", static_cast<Kind>(k), size, f, "size differs");
                double fms = 0;
                const uint32_t fs = CheckOne(ctx, src, static_cast<Kind>(k), f, ef, &fms);
                std::printf("  %-14s %5x %9u | %10u %9.2f | %10u %9.2f | %+6.1f%% %6.1fx\n", kKindName[k], f, size, gs, gms, fs, fms,
                            gs ? 100.0 * (static_cast<double>(fs) / gs - 1.0) : 0.0, fms > 0 ? gms / fms : 0.0);
                all.in += size;
                all.gameOut += gs;
                all.fastOut += fs;
                all.gameMs += gms;
                all.fastMs += fms;
            }
    std::printf("  total: %.1f MB in; game %.2f MB in %.0f ms; fast %.2f MB in %.0f ms: size %+.2f%%, %.1fx faster\n", all.in / 1048576.0, all.gameOut / 1048576.0, all.gameMs,
                all.fastOut / 1048576.0, all.fastMs, 100.0 * (static_cast<double>(all.fastOut) / all.gameOut - 1.0), all.gameMs / all.fastMs);

    // ---- 4. search depth sweep on a mixed set (flags 2) ----
    {
        std::printf("\nSearch depth (Effort::maxChain; the ASI uses 32, niceLen 96, lazy) on 64 KB and 256 KB buffers of every kind but random / zeros:\n");
        std::vector<std::vector<uint8_t>> set;
        uint64_t gameOut = 0, in = 0;
        double gameMs = 0;
        for (int k = 2; k < kKinds; k++)
            for (uint32_t size : {65536u, 262144u}) {
                std::vector<uint8_t> v;
                Generate(rng, static_cast<Kind>(k), size, v);
                const double t0 = NowMs();
                gameOut += GameCompress(v.data(), size, nullptr, 2);
                gameMs += NowMs() - t0;
                in += size;
                set.push_back(std::move(v));
            }
        std::printf("  game: %.2f MB -> %.2f MB in %.0f ms\n", in / 1048576.0, gameOut / 1048576.0, gameMs);
        for (int depth : {4, 8, 16, 32, 64, 128, 256}) {
            Effort e;
            e.maxChain = depth;
            uint64_t out = 0;
            double ms = 0;
            for (const auto& v : set) {
                const double t0 = NowMs();
                const uint32_t r = Compress(ctx, v.data(), static_cast<uint32_t>(v.size()), nullptr, 0, 2, e);
                ms += NowMs() - t0;
                out += r;
            }
            std::printf("  depth %3d: %.2f MB (%+.2f%% vs game) in %.0f ms (%.1fx faster)\n", depth, out / 1048576.0, 100.0 * (static_cast<double>(out) / gameOut - 1.0), ms,
                        ms > 0 ? gameMs / ms : 0.0);
        }
    }

    // ---- 6. segmented compression (large streams on several threads in the ASI) ----
    {
        std::vector<uint8_t> mem2;
        Context ctx2 = MakeContext(mem2);
        uint64_t n = 0;
        for (int k = 0; k < kKinds; k++)
            for (uint32_t size = 0; size <= 300; size += 7)
                for (uint32_t f : kFlags) {
                    Generate(rng, static_cast<Kind>(k), size, src);
                    CheckSegmented(ctx, ctx2, src, static_cast<Kind>(k), f, ef, nullptr);
                    n++;
                }
        // around the piece boundaries (a match must never cross one, the last 4 bytes stay literals)
        for (uint32_t pieces = 1; pieces <= 3; pieces++)
            for (int d = -9; d <= 9; d++)
                for (int k : {kZeros, kRuns, kPeriodic, kTexture, kFarRepeats, kText})
                    for (uint32_t f : kFlags) {
                        Generate(rng, static_cast<Kind>(k), static_cast<uint32_t>(static_cast<int>(pieces * kSegmentBytes) + d), src);
                        CheckSegmented(ctx, ctx2, src, static_cast<Kind>(k), f, ef, nullptr);
                        n++;
                    }
        const int buffers = quick ? 30 : 120;
        uint64_t bytes = 0;
        for (int i = 0; i < buffers; i++) {
            const Kind k = static_cast<Kind>(rng.Below(kKinds));
            const uint32_t size = static_cast<uint32_t>(std::exp(std::log(301.0) + (std::log(6291456.0) - std::log(301.0)) * (rng.Next() / 4294967296.0)));
            Generate(rng, k, size, src);
            CheckSegmented(ctx, ctx2, src, k, kFlags[rng.Below(4)], ef, nullptr);
            bytes += size;
            n++;
        }
        std::printf("\nSegmented: %llu streams checked (%d random up to 6 MB, %.1f MB), %llu failures so far\n", static_cast<unsigned long long>(n), buffers,
                    static_cast<double>(bytes) / 1048576.0, static_cast<unsigned long long>(g_fail));
        // size and time against Compress on 5.5 MB streams (flags 2, the caches); the threads: every piece parsed on its own
        std::printf("  %-14s %9s | %10s %9s | %10s %9s %s\n", "kind", "size", "Compress", "ms", "segmented", "ms (1 thread)", "size");
        for (int k = 2; k < kKinds; k++) {
            Generate(rng, static_cast<Kind>(k), 5600000, src);
            double cms = 0, sms = 0;
            const uint32_t cs = CheckOne(ctx, src, static_cast<Kind>(k), 2, ef, &cms);
            const uint32_t ss = CheckSegmented(ctx, ctx2, src, static_cast<Kind>(k), 2, ef, &sms);
            std::printf("  %-14s %9zu | %10u %9.1f | %10u %9.1f     %+.2f%%\n", kKindName[k], src.size(), cs, cms, ss, sms, cs ? 100.0 * (static_cast<double>(ss) / cs - 1.0) : 0.0);
        }
        // real threads (the ASI's rounds: 1 + 6 threads take the pieces of a round, then they are written in order)
        const uint32_t threads = 7;
        std::vector<std::vector<uint8_t>> mems(threads);
        std::vector<Context> ctxs(threads);
        for (uint32_t t = 0; t < threads; t++) ctxs[t] = MakeContext(mems[t]);
        std::vector<std::vector<Token>> tok(threads, std::vector<Token>(kMaxSegmentTokens));
        std::printf("  threaded (%u threads, rounds of %u pieces), same bytes as the serial segmented stream:\n", threads, threads);
        double serialAll = 0, threadedAll = 0;
        for (int k = 2; k < kKinds; k++) {
            Generate(rng, static_cast<Kind>(k), 5600000, src);
            const uint32_t size = static_cast<uint32_t>(src.size());
            std::vector<uint8_t> a(static_cast<size_t>(SizeBound(size)) + 64), b(a.size());
            double t0 = NowMs();
            const uint32_t ra = CompressSegmented(ctx, src.data(), size, a.data(), 0, 2, ef, g_tokA.data());
            const double serialMs = NowMs() - t0;
            t0 = NowMs();
            SegmentEncoder enc;
            enc.Begin(src.data(), size, b.data(), 0, 2);
            const uint32_t pieces = SegmentCount(size);
            std::vector<uint32_t> counts(threads);
            for (uint32_t first = 0; first < pieces; first += threads) {
                const uint32_t count = pieces - first < threads ? pieces - first : threads;
                std::atomic<uint32_t> next{0};
                auto work = [&](uint32_t t) {
                    for (;;) {
                        const uint32_t i = next.fetch_add(1);
                        if (i >= count) break;
                        counts[i] = ParseSegment(ctxs[t], src.data(), size, 2, first + i, ef, tok[i].data());
                    }
                };
                std::vector<std::thread> pool;
                for (uint32_t t = 1; t < count; t++) pool.emplace_back(work, t);
                work(0);
                for (auto& th : pool) th.join();
                for (uint32_t i = 0; i < count; i++) enc.Add(tok[i].data(), counts[i]);
            }
            const uint32_t rb = enc.End();
            const double threadedMs = NowMs() - t0;
            serialAll += serialMs;
            threadedAll += threadedMs;
            const bool same = ra == rb && std::memcmp(a.data(), b.data(), ra) == 0;
            if (!same) Fail("threaded segmented", static_cast<Kind>(k), size, 2, "other bytes than the serial run");
            std::printf("    %-14s serial %7.1f ms, threaded %6.1f ms (thread start included): %s\n", kKindName[k], serialMs, threadedMs, same ? "same bytes" : "DIFFERENT");
        }
        std::printf("    total: serial %.0f ms, threaded %.0f ms (%.1fx)\n", serialAll, threadedAll, threadedAll > 0 ? serialAll / threadedAll : 0.0);
    }

    // ---- 5. files ----
    for (const std::string& path : files) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
            std::printf("%s: cannot open\n", path.c_str());
            continue;
        }
        src.clear();
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) src.insert(src.end(), buf, buf + n);
        std::fclose(f);
        if (src.size() > 0x0F000000) {
            std::printf("%s: too large\n", path.c_str());
            continue;
        }
        for (uint32_t fl : {2u, 0u}) {
            double fms = 0;
            const uint32_t fs = CheckOne(ctx, src, kRandom, fl, ef, &fms);
            const double t0 = NowMs();
            const uint32_t gs = GameCompress(src.data(), static_cast<uint32_t>(src.size()), nullptr, fl);
            const double gms = NowMs() - t0;
            std::printf("%s flags %x: %zu bytes; game %u in %.1f ms; fast %u in %.1f ms (%+.1f%%, %.1fx)\n", path.c_str(), fl, src.size(), gs, gms, fs, fms,
                        gs ? 100.0 * (static_cast<double>(fs) / gs - 1.0) : 0.0, fms > 0 ? gms / fms : 0.0);
        }
    }

    std::printf(g_fail ? "\nRESULT: %llu FAILURES\n" : "\nRESULT: every round trip and stream check passed\n", static_cast<unsigned long long>(g_fail));
    return g_fail ? 1 : 0;
}
