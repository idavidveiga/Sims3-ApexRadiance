// Smoothed terrain light maps (part of Night Lighting)
//
// The world's lamp light on the ground comes from one 256x256 DXT5 light map per 256 m terrain chunk (1 texel per metre,
// see lot_light_bridge.cpp). Two visible problems come from that texture itself:
//  - blocky, "low resolution" lamp circles: 1 texel per metre stretched with plain bilinear filtering, plus the 4x4 DXT
//    blocks;
//  - purple / green specks at night: DXT stores colour as RGB565 endpoints, so dim light gets rounded to tinted values
//    (green has 6 bits, red and blue 5). On white snow this is very visible.
// Fix, texture side (no shader changes): every chunk light map the world draws is decoded, the colour noise is removed
// by blurring only the colour ratio (brightness stays sharp), and it is enlarged 4x with a cubic B-spline (smooth, no
// ringing) that reads across the neighbouring chunks, so there are no seams. The result (1024x1024 A8R8G8B8 with
// dithering and a full mip chain, in video memory) replaces the game's texture in the world terrain, lot and road draws.
// The game's maps are hashed round robin, so when it rebuilds the lighting (dusk, lights changing) the smoothed maps are
// rebuilt too. The heavy work runs on a worker thread.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "lightmap_smooth.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kSrc = 256;             // game light map size
constexpr int kScale = 4;             // enlargement
constexpr int kOut = kSrc * kScale;   // 1024
constexpr int kBorder = 6;            // texels read from the neighbouring chunks
constexpr int kExt = kSrc + 2 * kBorder;
constexpr int kChunkSize = 256;       // world units per chunk (key step)

using Plane = std::vector<float>;     // RGBA float, kSrc * kSrc * 4 (worker only)
using Raw = std::vector<uint8_t>;     // the game's level 0, DXT5, 64 KB (kept per chunk)
using RawPtr = std::shared_ptr<const Raw>;
constexpr int kPitch = kSrc / 4 * 16;  // DXT5 bytes per row of blocks

struct Entry {
    IDirect3DTexture9* src = nullptr;  // game texture (AddRef'd)
    uint64_t hash = 0;
    RawPtr raw;
    IDirect3DTexture9* smooth = nullptr; // D3DPOOL_DEFAULT
    bool dirty = true;
    bool inFlight = false;
    int gen = 0;
    uint32_t lastUse = 0; // frame of the last draw that asked for it (visible chunks are processed first)
};

struct Job {
    LightmapSmooth::Key key;
    int gen;
    RawPtr raw[9]; // [dz+1][dx+1], centre = 4
};

struct Result {
    LightmapSmooth::Key key;
    int gen;
    std::vector<std::vector<uint32_t>> levels;
};

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_failed{false}; // out of memory once: off for the rest of the session
std::map<LightmapSmooth::Key, Entry> g_entries; // render thread only
size_t g_checkCursor = 0;
int g_uploaded = 0;
int g_unreadable = 0; // game maps that could not be read (not managed 256x256 DXT5)
uint32_t g_frame = 0; // OnPresent counter, for Entry::lastUse
int g_genCounter = 0;

std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::vector<Result> g_results;
bool g_workerStarted = false; // detached worker (a joinable std::thread would terminate the game at exit)
bool g_stop = false;

// ---- DXT5 decode ----
void DecodeDxt5(const uint8_t* data, int pitch, Plane& out) {
    out.assign(static_cast<size_t>(kSrc) * kSrc * 4, 0.0f);
    for (int by = 0; by < kSrc / 4; by++) {
        const uint8_t* row = data + by * pitch;
        for (int bx = 0; bx < kSrc / 4; bx++) {
            const uint8_t* b = row + bx * 16;
            float a[8];
            a[0] = b[0] / 255.0f;
            a[1] = b[1] / 255.0f;
            if (b[0] > b[1]) {
                for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7.0f;
            } else {
                for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5.0f;
                a[6] = 0.0f;
                a[7] = 1.0f;
            }
            uint64_t aBits = 0;
            for (int i = 0; i < 6; i++) aBits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
            const uint16_t c0 = static_cast<uint16_t>(b[8] | (b[9] << 8));
            const uint16_t c1 = static_cast<uint16_t>(b[10] | (b[11] << 8));
            float col[4][3];
            auto unpack = [](uint16_t c, float* o) {
                o[0] = ((c >> 11) & 31) / 31.0f;
                o[1] = ((c >> 5) & 63) / 63.0f;
                o[2] = (c & 31) / 31.0f;
            };
            unpack(c0, col[0]);
            unpack(c1, col[1]);
            for (int k = 0; k < 3; k++) {
                col[2][k] = (2 * col[0][k] + col[1][k]) / 3.0f;
                col[3][k] = (col[0][k] + 2 * col[1][k]) / 3.0f;
            }
            const uint32_t cBits = b[12] | (b[13] << 8) | (b[14] << 16) | (static_cast<uint32_t>(b[15]) << 24);
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    const int i = py * 4 + px;
                    float* o = &out[((static_cast<size_t>(by) * 4 + py) * kSrc + bx * 4 + px) * 4];
                    const int ci = (cBits >> (2 * i)) & 3;
                    o[0] = col[ci][0];
                    o[1] = col[ci][1];
                    o[2] = col[ci][2];
                    o[3] = a[(aBits >> (3 * i)) & 7];
                }
        }
    }
}

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h | 1; // never 0 (0 = not read yet)
}

// Reads level 0 of a game light map. Returns false when it is not a lockable 256x256 DXT5.
bool ReadSource(IDirect3DTexture9* tex, std::vector<uint8_t>& raw, int& pitch) {
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d)) || d.Width != kSrc || d.Height != kSrc || d.Format != D3DFMT_DXT5 || d.Pool == D3DPOOL_DEFAULT) return false;
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return false;
    pitch = kSrc / 4 * 16;
    raw.resize(static_cast<size_t>(pitch) * (kSrc / 4));
    for (int r = 0; r < kSrc / 4; r++) std::memcpy(raw.data() + r * pitch, static_cast<const uint8_t*>(lr.pBits) + r * lr.Pitch, pitch);
    tex->UnlockRect(0);
    return true;
}

// ---- smoothing (worker thread) ----
void BSplineWeights(float t, float w[4]) {
    const float t2 = t * t, t3 = t2 * t, s = 1 - t;
    w[0] = s * s * s / 6.0f;
    w[1] = (3 * t3 - 6 * t2 + 4) / 6.0f;
    w[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.0f;
    w[3] = t3 / 6.0f;
}

Result Process(const Job& job) {
    // 1. Extended source (centre chunk + borders from the neighbours, clamped where a neighbour is missing).
    //    Chunks are decoded one at a time (only 64 KB of DXT5 is kept per chunk).
    std::vector<float> ext(static_cast<size_t>(kExt) * kExt * 4);
    Plane plane;
    for (int slot = 0; slot < 9; slot++) {
        const int sdx = slot % 3 - 1, sdz = slot / 3 - 1;
        if (!job.raw[slot]) continue;
        DecodeDxt5(job.raw[slot]->data(), kPitch, plane);
        for (int y = 0; y < kExt; y++)
            for (int x = 0; x < kExt; x++) {
                int sx = x - kBorder, sy = y - kBorder;
                const int dx = sx < 0 ? -1 : (sx >= kSrc ? 1 : 0);
                const int dz = sy < 0 ? -1 : (sy >= kSrc ? 1 : 0);
                const bool neighbour = job.raw[(dz + 1) * 3 + (dx + 1)] != nullptr;
                if (neighbour ? (dx != sdx || dz != sdz) : slot != 4) continue; // this texel comes from another slot
                if (neighbour) {
                    sx -= dx * kSrc;
                    sy -= dz * kSrc;
                } else { // missing neighbour: clamp to the centre chunk
                    sx = std::clamp(sx, 0, kSrc - 1);
                    sy = std::clamp(sy, 0, kSrc - 1);
                }
                std::memcpy(&ext[(static_cast<size_t>(y) * kExt + x) * 4], &plane[(static_cast<size_t>(sy) * kSrc + sx) * 4], 16);
            }
    }
    Plane().swap(plane);

    // 2. Brightness (luma) and alpha planes; colour ratio from a blurred copy (removes the RGB565 tint noise).
    const size_t n = static_cast<size_t>(kExt) * kExt;
    std::vector<float> Y(n), A(n), rgbB(n * 3), yB(n), tmp(n * 4);
    for (size_t i = 0; i < n; i++) {
        const float* c = &ext[i * 4];
        Y[i] = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
        A[i] = c[3];
    }
    const float g[7] = {0.0366f, 0.1112f, 0.2167f, 0.2710f, 0.2167f, 0.1112f, 0.0366f}; // sigma 1.5
    auto blur = [&](auto get, auto set) {
        for (int y = 0; y < kExt; y++)
            for (int x = 0; x < kExt; x++) {
                float s[4] = {};
                for (int k = -3; k <= 3; k++) {
                    const int xx = std::clamp(x + k, 0, kExt - 1);
                    float v[4];
                    get(static_cast<size_t>(y) * kExt + xx, v);
                    for (int c = 0; c < 4; c++) s[c] += g[k + 3] * v[c];
                }
                std::memcpy(&tmp[(static_cast<size_t>(y) * kExt + x) * 4], s, 16);
            }
        for (int y = 0; y < kExt; y++)
            for (int x = 0; x < kExt; x++) {
                float s[4] = {};
                for (int k = -3; k <= 3; k++) {
                    const int yy = std::clamp(y + k, 0, kExt - 1);
                    for (int c = 0; c < 4; c++) s[c] += g[k + 3] * tmp[(static_cast<size_t>(yy) * kExt + x) * 4 + c];
                }
                set(static_cast<size_t>(y) * kExt + x, s);
            }
    };
    blur([&](size_t i, float* v) { v[0] = ext[i * 4]; v[1] = ext[i * 4 + 1]; v[2] = ext[i * 4 + 2]; v[3] = Y[i]; },
         [&](size_t i, const float* s) { rgbB[i * 3] = s[0]; rgbB[i * 3 + 1] = s[1]; rgbB[i * 3 + 2] = s[2]; yB[i] = s[3]; });
    std::vector<float> chroma(n * 3);
    for (size_t i = 0; i < n; i++) {
        const float yb = yB[i];
        const float w = std::clamp(yb / 0.03f, 0.0f, 1.0f); // very dim: neutral (the noise is all there is)
        for (int c = 0; c < 3; c++) {
            const float ratio = yb > 1e-4f ? std::clamp(rgbB[i * 3 + c] / yb, 0.0f, 4.0f) : 1.0f;
            chroma[i * 3 + c] = 1.0f + (ratio - 1.0f) * w;
        }
    }

    // 3. 4x enlargement: cubic B-spline for brightness and alpha, bilinear for the (already smooth) colour ratio.
    //    Output texel j sits at source coordinate (j + 0.5) / 4 - 0.5.
    struct Tap { int i0; float w[4]; int l0; float lf; };
    std::vector<Tap> taps(kOut);
    for (int j = 0; j < kOut; j++) {
        const float s = (j + 0.5f) / kScale - 0.5f + kBorder;
        const float f = std::floor(s);
        taps[j].i0 = static_cast<int>(f) - 1;
        BSplineWeights(s - f, taps[j].w);
        taps[j].l0 = static_cast<int>(f);
        taps[j].lf = s - f;
    }
    // horizontal pass: kExt rows x kOut columns (Y, A, chroma rgb)
    std::vector<float> h(static_cast<size_t>(kExt) * kOut * 5);
    for (int y = 0; y < kExt; y++)
        for (int x = 0; x < kOut; x++) {
            const Tap& t = taps[x];
            float yy = 0, aa = 0;
            for (int k = 0; k < 4; k++) {
                const size_t i = static_cast<size_t>(y) * kExt + (t.i0 + k);
                yy += t.w[k] * Y[i];
                aa += t.w[k] * A[i];
            }
            float* o = &h[(static_cast<size_t>(y) * kOut + x) * 5];
            o[0] = yy;
            o[1] = aa;
            const size_t i0 = static_cast<size_t>(y) * kExt + t.l0;
            for (int c = 0; c < 3; c++) o[2 + c] = chroma[i0 * 3 + c] * (1 - t.lf) + chroma[(i0 + 1) * 3 + c] * t.lf;
        }
    static const float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    Result r{job.key, job.gen, {}};
    r.levels.emplace_back(static_cast<size_t>(kOut) * kOut);
    auto& l0 = r.levels[0];
    for (int y = 0; y < kOut; y++) {
        const Tap& t = taps[y];
        for (int x = 0; x < kOut; x++) {
            float yy = 0, aa = 0;
            for (int k = 0; k < 4; k++) {
                const float* p = &h[(static_cast<size_t>(t.i0 + k) * kOut + x) * 5];
                yy += t.w[k] * p[0];
                aa += t.w[k] * p[1];
            }
            const float* c0 = &h[(static_cast<size_t>(t.l0) * kOut + x) * 5];
            const float* c1 = &h[(static_cast<size_t>(t.l0 + 1) * kOut + x) * 5];
            const float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
            uint32_t px = 0;
            for (int c = 0; c < 3; c++) {
                const float ch = c0[2 + c] * (1 - t.lf) + c1[2 + c] * t.lf;
                const float v = std::clamp(yy * ch * 255.0f + d, 0.0f, 255.0f);
                px |= static_cast<uint32_t>(v + 0.5f) << (16 - 8 * c); // A8R8G8B8: R at bit 16, G 8, B 0
            }
            px |= static_cast<uint32_t>(std::clamp(aa * 255.0f + d, 0.0f, 255.0f) + 0.5f) << 24;
            l0[static_cast<size_t>(y) * kOut + x] = px;
        }
    }
    // 4. Mip chain (2x2 box).
    for (int size = kOut / 2; size >= 1; size /= 2) {
        const auto& prev = r.levels.back();
        std::vector<uint32_t> lv(static_cast<size_t>(size) * size);
        const int ps = size * 2;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                uint32_t s[4] = {};
                const uint32_t q[4] = {prev[(2 * y) * ps + 2 * x], prev[(2 * y) * ps + 2 * x + 1], prev[(2 * y + 1) * ps + 2 * x], prev[(2 * y + 1) * ps + 2 * x + 1]};
                for (uint32_t v : q)
                    for (int c = 0; c < 4; c++) s[c] += (v >> (8 * c)) & 255;
                uint32_t px = 0;
                for (int c = 0; c < 4; c++) px |= ((s[c] + 2) / 4) << (8 * c);
                lv[static_cast<size_t>(y) * size + x] = px;
            }
        r.levels.push_back(std::move(lv));
    }
    return r;
}

void WorkerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            // at most 2 finished maps waiting (5.6 MB each): the render thread uploads one per frame
            g_cv.wait(lk, [] { return g_stop || (!g_jobs.empty() && g_results.size() < 2); });
            if (g_stop) return;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        Result r;
        try {
            r = Process(job);
        } catch (...) { // out of memory in a 32-bit game: give up on this map, never take the game down
            r = Result{job.key, job.gen, {}};
        }
        std::lock_guard<std::mutex> lk(g_mx);
        g_results.push_back(std::move(r));
    }
}

void EnsureWorker() {
    if (g_workerStarted) return;
    try {
        std::thread(WorkerMain).detach();
        g_workerStarted = true;
    } catch (...) {
    }
}

void MarkDirtyAround(const LightmapSmooth::Key& k) {
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            auto it = g_entries.find({k.first + dx * kChunkSize, k.second + dz * kChunkSize});
            if (it != g_entries.end()) it->second.dirty = true;
        }
}

// Reads one game map, decodes it when it changed. Returns true when it was read.
bool CheckEntry(const LightmapSmooth::Key& key, Entry& e) {
    std::vector<uint8_t> raw;
    int pitch = 0;
    if (!ReadSource(e.src, raw, pitch)) return false;
    const uint64_t h = Fnv(raw.data(), raw.size());
    if (h == e.hash) return true;
    (void)pitch; // always kPitch (ReadSource copies rows tightly)
    e.hash = h;
    e.raw = std::make_shared<const Raw>(std::move(raw));
    MarkDirtyAround(key);
    return true;
}

// ---- World light atlas: every smoothed chunk map at 2 texels per metre (its mip 1) in one texture covering all chunks
// seen, so surfaces that only know their world position (snowy floor tiles) can read the terrain light anywhere,
// across chunk borders. A render-target texture in video memory (ColorFill clears it); chunks are copied in with
// UpdateSurface from a small system-memory staging texture. Rebuilt (all chunks reprocessed) when the world grows.
constexpr int kAtlasPerChunk = kOut / 2; // 512 texels per 256 m chunk
constexpr int kAtlasMaxChunks = 16;      // 8192 texels
IDirect3DTexture9* g_atlas = nullptr;
IDirect3DTexture9* g_atlasStaging = nullptr;
int g_atlasMinX = 0, g_atlasMinZ = 0, g_atlasW = 0, g_atlasH = 0;
int g_atlasChunks = 0;

int ChunkIndex(int key) { return (key - kChunkSize / 2) / kChunkSize; } // key = chunk centre = 256 * i + 128

void ReleaseAtlas() {
    if (g_atlas) g_atlas->Release();
    if (g_atlasStaging) g_atlasStaging->Release();
    g_atlas = g_atlasStaging = nullptr;
    g_atlasW = g_atlasH = 0;
    g_atlasChunks = 0;
}

void EnsureAtlas(IDirect3DDevice9* dev) {
    if (g_entries.empty()) return;
    int minX = INT_MAX, minZ = INT_MAX, maxX = INT_MIN, maxZ = INT_MIN;
    for (auto& [k, e] : g_entries) {
        minX = std::min(minX, ChunkIndex(k.first));
        maxX = std::max(maxX, ChunkIndex(k.first));
        minZ = std::min(minZ, ChunkIndex(k.second));
        maxZ = std::max(maxZ, ChunkIndex(k.second));
    }
    if (g_atlas && minX >= g_atlasMinX && minZ >= g_atlasMinZ && maxX < g_atlasMinX + g_atlasW && maxZ < g_atlasMinZ + g_atlasH) return;
    // grow to cover everything, with a chunk of margin so camera moves do not rebuild it every time
    if (g_atlas) {
        minX = std::min(minX, g_atlasMinX);
        minZ = std::min(minZ, g_atlasMinZ);
        maxX = std::max(maxX, g_atlasMinX + g_atlasW - 1);
        maxZ = std::max(maxZ, g_atlasMinZ + g_atlasH - 1);
    }
    minX--;
    minZ--;
    maxX++;
    maxZ++;
    ReleaseAtlas();
    const int w = maxX - minX + 1, h = maxZ - minZ + 1;
    if (w > kAtlasMaxChunks || h > kAtlasMaxChunks) return; // unusually large world: no atlas
    if (FAILED(dev->CreateTexture(w * kAtlasPerChunk, h * kAtlasPerChunk, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_atlas, nullptr)) ||
        FAILED(dev->CreateTexture(kAtlasPerChunk, kAtlasPerChunk, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_atlasStaging, nullptr))) {
        ReleaseAtlas();
        return;
    }
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(g_atlas->GetSurfaceLevel(0, &s))) {
        dev->ColorFill(s, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        s->Release();
    }
    g_atlasMinX = minX;
    g_atlasMinZ = minZ;
    g_atlasW = w;
    g_atlasH = h;
    for (auto& [k, e] : g_entries) e.dirty = true; // refill every chunk
}

void AtlasUpload(IDirect3DDevice9* dev, const LightmapSmooth::Key& key, const std::vector<uint32_t>& level) {
    if (!g_atlas || !g_atlasStaging || level.size() != static_cast<size_t>(kAtlasPerChunk) * kAtlasPerChunk) return;
    const int ix = ChunkIndex(key.first) - g_atlasMinX, iz = ChunkIndex(key.second) - g_atlasMinZ;
    if (ix < 0 || iz < 0 || ix >= g_atlasW || iz >= g_atlasH) return;
    D3DLOCKED_RECT lr{};
    if (FAILED(g_atlasStaging->LockRect(0, &lr, nullptr, 0))) return;
    for (int y = 0; y < kAtlasPerChunk; y++)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &level[static_cast<size_t>(y) * kAtlasPerChunk], kAtlasPerChunk * 4);
    g_atlasStaging->UnlockRect(0);
    IDirect3DSurface9 *src = nullptr, *dst = nullptr;
    if (SUCCEEDED(g_atlasStaging->GetSurfaceLevel(0, &src)) && SUCCEEDED(g_atlas->GetSurfaceLevel(0, &dst))) {
        const POINT pt{ix * kAtlasPerChunk, iz * kAtlasPerChunk};
        if (SUCCEEDED(dev->UpdateSurface(src, nullptr, dst, &pt))) g_atlasChunks++;
    }
    if (src) src->Release();
    if (dst) dst->Release();
}

void Upload(IDirect3DDevice9* dev, Result& r) {
    auto it = g_entries.find(r.key);
    if (it == g_entries.end()) return;
    Entry& e = it->second;
    if (r.gen != e.gen) return; // from before a Clear or an older job
    e.inFlight = false;
    if (r.levels.empty()) return; // processing failed
    const UINT levels = static_cast<UINT>(r.levels.size());
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(kOut, kOut, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr))) return;
    bool ok = true;
    for (UINT l = 0; l < levels && ok; l++) {
        D3DLOCKED_RECT lr{};
        if (FAILED(staging->LockRect(l, &lr, nullptr, 0))) {
            ok = false;
            break;
        }
        const int size = kOut >> l;
        for (int y = 0; y < size; y++) std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &r.levels[l][static_cast<size_t>(y) * size], size * 4);
        staging->UnlockRect(l);
    }
    if (ok && SUCCEEDED(dev->CreateTexture(kOut, kOut, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr)) &&
        SUCCEEDED(dev->UpdateTexture(staging, tex))) {
        if (e.smooth) e.smooth->Release();
        e.smooth = tex;
        g_uploaded++;
        if (r.levels.size() > 1) AtlasUpload(dev, r.key, r.levels[1]);
    } else if (tex)
        tex->Release();
    staging->Release();
}

} // namespace

namespace LightmapSmooth {

void SetEnabled(bool on) {
    if (g_enabled.exchange(on) != on && !on) Clear();
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed); }

IDirect3DTexture9* Get(const Key& key, IDirect3DTexture9* original) {
    if (!Enabled() || !original) return nullptr;
    Entry& e = g_entries[key];
    e.lastUse = g_frame;
    if (e.src != original) {
        if (e.src) e.src->Release();
        e.src = original;
        e.src->AddRef();
        e.hash = 0;
    }
    return e.smooth;
}

IDirect3DTexture9* Atlas(float c[4]) {
    if (!Enabled() || !g_atlas || g_atlasChunks == 0) return nullptr;
    // uv = world xz * c.xy + c.zw; the atlas starts at chunk (minX, minZ), i.e. world 256 * min
    const float sx = 1.0f / static_cast<float>(g_atlasW * kChunkSize), sz = 1.0f / static_cast<float>(g_atlasH * kChunkSize);
    c[0] = sx;
    c[1] = sz;
    c[2] = -static_cast<float>(g_atlasMinX * kChunkSize) * sx;
    c[3] = -static_cast<float>(g_atlasMinZ * kChunkSize) * sz;
    return g_atlas;
}

IDirect3DTexture9* Find(const Key& key) {
    if (!Enabled()) return nullptr;
    auto it = g_entries.find(key);
    return it == g_entries.end() ? nullptr : it->second.smooth;
}

static void OnPresentBody(IDirect3DDevice9* dev) {
    if (!Enabled() || g_entries.empty()) return;
    EnsureWorker();
    g_frame++;
    // Chunks in view first: order by the last frame a draw asked for them (then by key, for a stable order).
    std::vector<std::pair<const LightmapSmooth::Key, Entry>*> order;
    order.reserve(g_entries.size());
    for (auto& kv : g_entries) order.push_back(&kv);
    std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->second.lastUse > b->second.lastUse; });
    // New maps first (up to 4 per frame), then one changed-check per frame.
    int reads = 0;
    for (auto* kv : order) {
        const auto& k = kv->first;
        Entry& e = kv->second;
        if (e.hash == 0 && reads < 4) {
            if (CheckEntry(k, e)) reads++;
            else {
                // Not readable (not a managed 256x256 DXT5): never smoothed. If a smoothed map of an OLDER source is
                // still here (the game swapped in a new map, e.g. after rebuilding the lighting), drop it so the draw
                // uses the game's current map instead of a stale one (review 25/09, winter seam m73/m74).
                e.hash = 1;
                D3DSURFACE_DESC d{};
                if (e.src) e.src->GetLevelDesc(0, &d);
                LOG_INFO(std::format("[LightmapSmooth] Map ({}, {}) unreadable: {}x{} format {} pool {} levels {}{}", k.first, k.second, d.Width, d.Height,
                                     static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Pool), e.src ? e.src->GetLevelCount() : 0,
                                     e.smooth ? " (old smoothed map dropped)" : ""));
                if (e.smooth) {
                    e.smooth->Release();
                    e.smooth = nullptr;
                }
                g_unreadable++;
            }
        }
    }
    if (reads == 0) {
        g_checkCursor = (g_checkCursor + 1) % g_entries.size();
        auto it = g_entries.begin();
        std::advance(it, g_checkCursor);
        CheckEntry(it->first, it->second);
    }
    EnsureAtlas(dev);
    // Queue work, chunks in view first.
    for (auto* kv : order) {
        const auto& k = kv->first;
        Entry& e = kv->second;
        if (!e.dirty || e.inFlight || !e.raw) continue;
        Job job{k, e.gen = ++g_genCounter, {}};
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                auto n = g_entries.find({k.first + dx * kChunkSize, k.second + dz * kChunkSize});
                if (n != g_entries.end()) job.raw[(dz + 1) * 3 + (dx + 1)] = n->second.raw;
            }
        e.dirty = false;
        e.inFlight = true;
        std::lock_guard<std::mutex> lk(g_mx);
        g_jobs.push_back(std::move(job));
        g_cv.notify_one();
    }
    // Upload one finished map per frame.
    Result r;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_results.empty()) {
            r = std::move(g_results.front());
            g_results.erase(g_results.begin());
            have = true;
        }
    }
    if (have) g_cv.notify_one(); // room for the next finished map
    if (have) Upload(dev, r);
}

void OnPresent(IDirect3DDevice9* dev) {
    try {
        OnPresentBody(dev);
    } catch (...) { // out of memory: turn the feature off instead of taking the game down
        LOG_WARNING("[LightmapSmooth] Out of memory: smoothed light map off");
        g_failed = true;
        Clear();
    }
}

void OnPreReset(IDirect3DDevice9*) {
    ReleaseAtlas();
    for (auto& [k, e] : g_entries) {
        if (e.smooth) {
            e.smooth->Release();
            e.smooth = nullptr;
        }
        e.dirty = true; // rebuilt after the reset
    }
}

void Clear() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_jobs.clear();
        g_results.clear();
    }
    g_cv.notify_all();
    for (auto& [k, e] : g_entries) {
        if (e.src) e.src->Release();
        if (e.smooth) e.smooth->Release();
    }
    g_entries.clear();
    g_checkCursor = 0;
    ReleaseAtlas();
}

std::string Status() {
    if (!Enabled()) return "desligado";
    int ready = 0, pending = 0;
    for (auto& [k, e] : g_entries) {
        if (e.smooth) ready++;
        if (e.dirty || e.inFlight) pending++;
    }
    return std::format("chunks smoothed: {} of {} | queued: {} | uploaded: {} | unreadable: {} | world map: {}x{} chunks ({} copies)", ready, g_entries.size(), pending,
                       g_uploaded, g_unreadable, g_atlasW, g_atlasH, g_atlasChunks);
}

} // namespace LightmapSmooth
