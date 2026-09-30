// Offline check of features/cas_tri_sort.{h,cpp}: the fast CAS triangle sort against the literal translation of the game's
// function (TS3W.exe 0x005D1960, CasTriSort::Ref):
//   1. every triangle's count, Fast (split over 1..8 ranges and threads) against Ref, on meshes packed the way the game
//      packs positions (FUN_005d1010 case 1: 16-bit x, y, z and w = 0x7FFD / ceil(max |coordinate|)), and on odd data
//      (w = 0 or negative, repeated vertices, degenerate and zero-area triangles, all-equal positions, index lists not a
//      multiple of 3, vertex counts over 65536 where the game reads vertex v & 0xFFFF);
//   2. the sorted index lists of both, byte for byte (and the indices past the last whole triangle untouched);
//   3. time of Ref, Fast on one thread and Fast on 8 threads, on meshes of the size seen in game.
// Console output only; writes no files. In game the ASI also checks its result against the game's own function.
//
// Build (x86 like the game), from "x86 Native Tools Command Prompt for VS 2022" or after vcvars32.bat:
//   cd /d C:\Users\luiz_\Desktop\S3SS-dev\S3SSApex\tools\cas_sort_test
//   cl /nologo /O2 /EHsc /std:c++20 /I..\..\features cas_sort_test.cpp ..\..\features\cas_tri_sort.cpp /Fe:cas_sort_test.exe
// Run: cas_sort_test.exe [--quick]. Exit code 0 = every count and every sorted list equal.
//
// Part of Apex Radiance. Credits: @loinyx
#include "cas_tri_sort.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace CasTriSort;

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
    float Unit() { return static_cast<float>(Next() >> 8) / 16777216.0f; }
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

struct Mesh {
    std::vector<uint8_t> vb;
    std::vector<uint16_t> ib;
    uint32_t vertices = 0, stride = 0, offset = 0;
    Input In() const {
        Input in;
        in.indices = ib.data();
        in.vertices = vb.data();
        in.indexCount = static_cast<uint32_t>(ib.size());
        in.vertexCount = vertices;
        in.stride = stride;
        in.positionOffset = offset;
        return in;
    }
};

void PutShort(uint8_t* p, int v) {
    const int16_t s = static_cast<int16_t>(v);
    std::memcpy(p, &s, 2);
}

enum Kind { kPacked, kOdd, kFlat, kKinds };
const char* const kKindName[kKinds] = {"packed like the game", "odd values", "all positions equal"};

// A mesh: vertex data (stride, offset), `vertices` vertices (the buffer holds max(vertices, 65536 when larger)), triangles
Mesh Make(Rng& r, Kind k, uint32_t vertices, uint32_t indexCount) {
    Mesh m;
    m.vertices = vertices;
    m.offset = r.Below(3) * 4;
    m.stride = m.offset + 8 + r.Below(5) * 4;
    const uint32_t stored = vertices > 65536 ? 65536 : vertices;
    m.vb.assign(static_cast<size_t>(stored) * m.stride + 16, 0);
    // a blob of points (a Sim part is a few units across), quantized as FUN_005d1010 case 1 does
    const float extent = 0.2f + r.Unit() * 2.5f;
    const int scale = 0x7FFD / static_cast<int>(std::ceil(extent));
    for (uint32_t v = 0; v < stored; v++) {
        uint8_t* p = m.vb.data() + static_cast<size_t>(v) * m.stride + m.offset;
        if (k == kFlat) {
            PutShort(p, 100), PutShort(p + 2, -200), PutShort(p + 4, 300), PutShort(p + 6, scale);
            continue;
        }
        const float a = r.Unit() * 6.2831853f, b = r.Unit() * 3.1415926f, rad = extent * (0.6f + 0.4f * r.Unit());
        const float x = rad * std::sin(b) * std::cos(a), y = rad * std::cos(b), z = rad * std::sin(b) * std::sin(a);
        int w = scale;
        if (k == kOdd) {
            const uint32_t o = r.Below(40);
            if (o == 0) w = 0;
            else if (o == 1) w = -scale;
            else if (o == 2) w = 1;
            if (r.Below(30) == 0 && v > 0) { // a repeated vertex
                std::memcpy(p, m.vb.data() + static_cast<size_t>(r.Below(v)) * m.stride + m.offset, 8);
                continue;
            }
            if (r.Below(50) == 0) { // extreme shorts
                PutShort(p, r.Below(2) ? 32767 : -32768), PutShort(p + 2, r.Below(2) ? 32767 : -32768), PutShort(p + 4, 0), PutShort(p + 6, w);
                continue;
            }
        }
        PutShort(p, static_cast<int>(std::floor(x * scale + 0.5f))), PutShort(p + 2, static_cast<int>(std::floor(y * scale + 0.5f))),
            PutShort(p + 4, static_cast<int>(std::floor(z * scale + 0.5f))), PutShort(p + 6, w);
    }
    m.ib.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) m.ib[i] = static_cast<uint16_t>(r.Below(stored));
    if (k == kOdd)
        for (uint32_t t = 0; t + 3 <= indexCount; t += 3) {
            const uint32_t o = r.Below(20);
            if (o == 0) m.ib[t + 1] = m.ib[t + 2] = m.ib[t]; // one point
            else if (o == 1) m.ib[t + 2] = m.ib[t + 1];      // a line
        }
    return m;
}

uint64_t g_fail = 0;
int g_printed = 0;
void Fail(const char* what, const Mesh& m, const char* kind, const char* detail) {
    g_fail++;
    if (g_printed++ < 20) std::printf("  FAIL %s: %s, %u vertices, %zu indices, stride %u: %s\n", what, kind, m.vertices, m.ib.size(), m.stride, detail);
}

// Fast with `threads` ranges (real threads when > 1)
std::vector<uint16_t> FastSort(const Mesh& m, uint32_t threads, std::vector<uint32_t>* countsOut, double* msPrepare = nullptr, double* msCount = nullptr) {
    const Input in = m.In();
    std::vector<uint16_t> out = m.ib;
    const double t0 = NowMs();
    Fast::Prepared p;
    Fast::Prepare(in, p);
    const double t1 = NowMs();
    const uint32_t n = Triangles(in);
    std::vector<uint32_t> counts(n);
    if (threads <= 1) {
        Fast::CountRange(in, p, 0, n, counts.data());
    } else {
        std::vector<std::thread> pool;
        std::atomic<uint32_t> next{0};
        const uint32_t chunk = n / (threads * 4) + 1;
        auto work = [&] {
            for (;;) {
                const uint32_t a = next.fetch_add(chunk);
                if (a >= n) break;
                Fast::CountRange(in, p, a, a + chunk < n ? a + chunk : n, counts.data());
            }
        };
        for (uint32_t k = 1; k < threads; k++) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
    }
    const double t2 = NowMs();
    Fast::Finish(in, counts.data(), out.data());
    if (msPrepare) *msPrepare += t1 - t0;
    if (msCount) *msCount += t2 - t1;
    if (countsOut) *countsOut = counts;
    return out;
}

void Check(const Mesh& m, const char* kind, uint32_t threads) {
    const Input in = m.In();
    const uint32_t n = Triangles(in);
    std::vector<uint32_t> fastCounts;
    const std::vector<uint16_t> fast = FastSort(m, threads, &fastCounts);
    for (uint32_t t = 0; t < n; t++) {
        const uint32_t ref = Ref::Count(in, t);
        if (ref != fastCounts[t]) {
            char d[96];
            std::snprintf(d, sizeof d, "triangle %u: Ref %u, Fast %u", t, ref, fastCounts[t]);
            return Fail("count", m, kind, d);
        }
    }
    std::vector<uint16_t> ref = m.ib;
    Ref::Sort(in, ref.data());
    if (ref != fast) return Fail("sorted indices", m, kind, "the lists differ");
    for (size_t i = 3 * static_cast<size_t>(n); i < m.ib.size(); i++)
        if (fast[i] != m.ib[i]) return Fail("tail", m, kind, "an index past the last whole triangle changed");
}

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    std::printf("CAS triangle sort test: Fast against the translation of TS3W.exe 0x005D1960 (Ref)\n");
    Rng r{0xCA5C0FFEEull};
    // ---- 1 + 2: counts and sorted lists ----
    uint64_t meshes = 0;
    for (int k = 0; k < kKinds; k++)
        for (uint32_t v : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 31u, 64u, 65u, 257u}) {
            for (uint32_t idx : {0u, 1u, 2u, 3u, 5u, 6u, 30u, 31u, 32u, 300u}) {
                const Mesh m = Make(r, static_cast<Kind>(k), v, idx);
                Check(m, kKindName[k], 1 + r.Below(3));
                meshes++;
            }
        }
    const int random = quick ? 40 : 200;
    for (int i = 0; i < random; i++) {
        const Kind k = static_cast<Kind>(r.Below(kKinds));
        const uint32_t v = 1 + r.Below(r.Below(4) ? 3000 : 12000);
        const uint32_t idx = r.Below(r.Below(4) ? 6000 : 20000);
        const Mesh m = Make(r, k, v, idx);
        Check(m, kKindName[k], 1 + r.Below(8));
        meshes++;
    }
    // vertex counts over 65536 (the game reads v & 0xFFFF): few triangles, Ref is slow
    for (uint32_t v : {65535u, 65536u, 65537u, 70001u, 131073u}) {
        const Mesh m = Make(r, kPacked, v, 3 * 4);
        Check(m, "packed, over 65536 vertices", 3);
        meshes++;
    }
    std::printf("Counts and sorted lists: %llu meshes, %llu failures\n", static_cast<unsigned long long>(meshes), static_cast<unsigned long long>(g_fail));

    // ---- 3: time ----
    std::printf("\nTime (meshes of the size seen in game):\n");
    for (auto [v, tris] : {std::pair<uint32_t, uint32_t>{1030, 1500}, {2013, 3000}, {3363, 5000}}) {
        const Mesh m = Make(r, kPacked, v, 3 * tris);
        const Input in = m.In();
        std::vector<uint16_t> ref = m.ib;
        double t0 = NowMs();
        Ref::Sort(in, ref.data());
        const double refMs = NowMs() - t0;
        double prep1 = 0, count1 = 0, prep8 = 0, count8 = 0;
        const std::vector<uint16_t> f1 = FastSort(m, 1, nullptr, &prep1, &count1);
        const std::vector<uint16_t> f8 = FastSort(m, 8, nullptr, &prep8, &count8);
        const bool same = f1 == ref && f8 == ref;
        if (!same) Fail("timed mesh", m, "packed", "the lists differ");
        std::printf("  %5u vertices, %5u triangles: Ref %7.1f ms | Fast 1 thread %6.2f ms (prepare %.2f) | Fast 8 threads %6.2f ms | %s\n", v, tris, refMs,
                    prep1 + count1, prep1, prep8 + count8, same ? "same" : "DIFFERENT");
    }
    std::printf(g_fail ? "\nRESULT: %llu FAILURES\n" : "\nRESULT: every count and every sorted list equal\n", static_cast<unsigned long long>(g_fail));
    return g_fail ? 1 : 0;
}
