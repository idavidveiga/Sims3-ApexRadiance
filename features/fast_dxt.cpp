// Faster texture compression: the game's CPU DXT1 / DXT5 encoders replaced by a bit-identical four-blocks-at-a-time
// version (see fast_dxt.h, features/dxt_codec.h and docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_dxt.h"
#include "dxt_codec.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <vector>

namespace FastDxt {
namespace {

using EntryChain::Layer;
using EntryChain::Site;
using FnCdecl2 = uint64_t(__cdecl*)(uint32_t, uint32_t);

static_assert(sizeof(DxtCodec::Dst) == 0x14 && sizeof(DxtCodec::Src) == 0x14, "the game's image descriptors are 0x14 bytes");

constexpr uint32_t kStartupChecks = 16; // the first images of each session are checked against the game on every build
constexpr uint64_t kMaxCheckBytes = 64ull << 20;

std::mutex g_ctrl;
bool g_started = false; // guarded by g_ctrl
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 8};
std::atomic<uint64_t> g_verifyAllUntil{0};
std::atomic<uint32_t> g_seq{0};
std::atomic<uint64_t> c_images{0}, c_pixels{0}, c_blocks{0}, c_delegated{0}, c_power{0}, c_solid{0}, c_passed{0};
std::atomic<uint64_t> c_checked{0}, c_mismatch{0}, c_notCheckable{0};
std::atomic<uint64_t> g_fastTicks{0}, g_checkGameTicks{0}, g_checkFastTicks{0};
double g_qpcMs = 0.0;
std::mutex g_mismatchLock;
std::string g_lastMismatch; // guarded by g_mismatchLock

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

struct FallbackCtx {
    FnCdecl2 game;
};

// One block through the game's own encoder: a one-block image (cols x rows pixels) at the block's pixels, same pitch and
// format. The game's drivers fetch an edge block exactly like that inside a whole image (same helper, same cols / rows).
void GameBlock(void* ctx, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out) {
    DxtCodec::Dst d{out, cols, rows, dxt5 ? 16u : 8u, 0};
    DxtCodec::Src s{src, 0, 0, pitch, format};
    static_cast<FallbackCtx*>(ctx)->game(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&d)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s)));
}

void RunFast(bool dxt5, const DxtCodec::Dst* d, const DxtCodec::Src* s, FnCdecl2 game) {
    FallbackCtx f{game};
    DxtCodec::FastCounters c;
    if (dxt5) DxtCodec::Fast::EncodeDxt5(d, s, &GameBlock, &f, &c);
    else DxtCodec::Fast::EncodeDxt1(d, s, &GameBlock, &f, &c);
    c_blocks.fetch_add(c.blocks, std::memory_order_relaxed);
    c_delegated.fetch_add(c.delegated, std::memory_order_relaxed);
    c_power.fetch_add(c.powerAxis, std::memory_order_relaxed);
    c_solid.fetch_add(c.solid, std::memory_order_relaxed);
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += std::format("{:02x}", static_cast<unsigned>(p[i]));
    return s;
}

uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Encodes with the game into a scratch copy and with Apex into the destination, then compares every block. On a
// difference the game's bytes are kept and the feature turns itself off for the session.
void Checked(bool dxt5, const DxtCodec::Dst* d, const DxtCodec::Src* s, FnCdecl2 game) {
    const uint32_t bs = dxt5 ? 16u : 8u;
    const uint32_t bx = (d->width >> 2) + ((d->width & 3) ? 1u : 0u), by = (d->height >> 2) + ((d->height & 3) ? 1u : 0u);
    const uint64_t rowBytes = static_cast<uint64_t>(bx) * bs;
    const uint64_t total = static_cast<uint64_t>(d->pitch) * by;
    std::vector<uint8_t> scratch;
    bool can = bx && by && d->pitch >= rowBytes && total <= kMaxCheckBytes;
    if (can) {
        try {
            scratch.resize(static_cast<size_t>(total));
        } catch (const std::bad_alloc&) {
            can = false;
        }
    }
    if (!can) {
        c_notCheckable.fetch_add(1, std::memory_order_relaxed);
        const uint64_t t0 = Qpc();
        RunFast(dxt5, d, s, game);
        g_fastTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
        return;
    }
    DxtCodec::Dst gd = *d;
    gd.ptr = scratch.data();
    const uint64_t t0 = Qpc();
    game(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&gd)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)));
    const uint64_t t1 = Qpc();
    RunFast(dxt5, d, s, game);
    const uint64_t t2 = Qpc();
    g_fastTicks.fetch_add(t2 - t1, std::memory_order_relaxed);
    for (uint32_t y = 0; y < by; y++) {
        const uint8_t* a = scratch.data() + static_cast<size_t>(y) * d->pitch;
        const uint8_t* b = d->ptr + static_cast<size_t>(y) * d->pitch;
        if (std::memcmp(a, b, static_cast<size_t>(rowBytes)) == 0) continue;
        uint32_t x = 0;
        while (x + 1 < bx && std::memcmp(a + x * bs, b + x * bs, bs) == 0) x++;
        const uint32_t cols = x < (d->width >> 2) ? 4u : (d->width & 3u);
        const uint32_t rows = d->height - 4 * y < 4 ? d->height - 4 * y : 4u;
        std::string px;
        for (uint32_t r = 0; r < rows; r++) {
            const uint8_t* row = s->ptr + static_cast<intptr_t>(s->pitch) * static_cast<intptr_t>(4 * y + r) + 16 * static_cast<uintptr_t>(x);
            for (uint32_t c = 0; c < cols; c++) px += std::format("{:08x} ", Load32(row + 4 * c));
            if (r + 1 < rows) px += "| ";
        }
        const std::string msg = std::format("DXT{} {}x{} image (format {:#x}), block ({}, {}), {}x{} pixels: game {} / Apex {}; pixels 0xAARRGGBB: {}", dxt5 ? 5 : 1,
                                            d->width, d->height, s->format, x, y, cols, rows, Hex(a + x * bs, bs), Hex(b + x * bs, bs), px);
        LOG_ERROR("[FastDxt] Verification mismatch: " + msg + ". The game's bytes were used; the feature turns itself off for this session.");
        for (uint32_t r = 0; r < by; r++) std::memcpy(d->ptr + static_cast<size_t>(r) * d->pitch, scratch.data() + static_cast<size_t>(r) * d->pitch, static_cast<size_t>(rowBytes));
        {
            std::lock_guard<std::mutex> lock(g_mismatchLock);
            g_lastMismatch = msg;
        }
        c_mismatch.fetch_add(1, std::memory_order_relaxed);
        g_selfDisabled.store(true);
        return;
    }
    g_checkGameTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
    g_checkFastTicks.fetch_add(t2 - t1, std::memory_order_relaxed);
    c_checked.fetch_add(1, std::memory_order_relaxed);
}

// The replacement of both encoders (layer FastDxt of the entry chain): cdecl(dst*, src*), eax = width & ~3 like the game
uint64_t Encode(bool dxt5, uint32_t dstp, uint32_t srcp) {
    const FnCdecl2 game = reinterpret_cast<FnCdecl2>(EntryChain::Next(dxt5 ? Site::DxtEncode5 : Site::DxtEncode1, Layer::FastDxt));
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed)) {
        c_passed.fetch_add(1, std::memory_order_relaxed);
        return game(dstp, srcp);
    }
    const auto* d = reinterpret_cast<const DxtCodec::Dst*>(static_cast<uintptr_t>(dstp));
    const auto* s = reinterpret_cast<const DxtCodec::Src*>(static_cast<uintptr_t>(srcp));
    if (dxt5 && s->format != 0x3D && s->format != 0x3E) return game(dstp, srcp); // the game encodes nothing for other formats
    const uint32_t ret = d->width & ~3u;
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    bool check = seq < kStartupChecks;
    if (!check && !kPublicBuild) {
        const int n = g_verifyEvery.load(std::memory_order_relaxed);
        check = (n > 0 && seq % static_cast<uint32_t>(n) == 0) || GetTickCount64() < g_verifyAllUntil.load(std::memory_order_relaxed);
    }
    c_images.fetch_add(1, std::memory_order_relaxed);
    c_pixels.fetch_add(static_cast<uint64_t>(d->width) * d->height, std::memory_order_relaxed);
    if (check) {
        Checked(dxt5, d, s, game);
    } else {
        const uint64_t t0 = Qpc();
        RunFast(dxt5, d, s, game);
        g_fastTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
    }
    return ret;
}

uint64_t __cdecl Hook_Dxt1(uint32_t dst, uint32_t src) { return Encode(false, dst, src); }
uint64_t __cdecl Hook_Dxt5(uint32_t dst, uint32_t src) { return Encode(true, dst, src); }

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!DxtCodec::CpuHasSse2()) return fail("This processor has no SSE2");
    std::string missing;
    if (!GameAddr::GroupAvailable("FastTextureCompression", &missing)) return fail(GameAddr::NotAvailable(missing));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    std::string err;
    if (!EntryChain::Install(Site::DxtEncode1, Layer::FastDxt, reinterpret_cast<void*>(&Hook_Dxt1), &err)) return fail("Could not hook the DXT1 encoder: " + err);
    if (!EntryChain::Install(Site::DxtEncode5, Layer::FastDxt, reinterpret_cast<void*>(&Hook_Dxt5), &err)) {
        EntryChain::Remove(Site::DxtEncode1, Layer::FastDxt);
        return fail("Could not hook the DXT5 encoder: " + err);
    }
    g_selfDisabled.store(false);
    g_seq.store(0); // the startup checks run again after every start
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[FastDxt] On: DXT1 {:#010x} and DXT5 {:#010x} encoders replaced (CPU: {}); the first {} images are checked against the game", EntryChain::GameFunction(Site::DxtEncode1),
                         EntryChain::GameFunction(Site::DxtEncode5), DxtCodec::CpuFeatureText(), kStartupChecks));
    return true;
}

bool Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return true;
    g_on.store(false, std::memory_order_release); // the hooks pass every call through from now on
    const bool a = EntryChain::Remove(Site::DxtEncode1, Layer::FastDxt);
    const bool b = EntryChain::Remove(Site::DxtEncode5, Layer::FastDxt);
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastDxt] Off ({} images, {} blocks, {} encoded by the game's function, {} checked, {} different){}", s.images, s.blocks, s.delegated, s.checked,
                         s.mismatches, a && b ? "" : "; an entry could not be put back (the hook stays and passes every call through)"));
    return a && b;
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }
int VerifyEvery() { return g_verifyEvery.load(); }
void VerifyAllFor(double seconds) { g_verifyAllUntil.store(GetTickCount64() + static_cast<uint64_t>(seconds * 1000.0)); }

Stats GetStats() {
    Stats s;
    s.images = c_images.load();
    s.pixels = c_pixels.load();
    s.blocks = c_blocks.load();
    s.delegated = c_delegated.load();
    s.powerAxis = c_power.load();
    s.solid = c_solid.load();
    s.passedThrough = c_passed.load();
    s.checked = c_checked.load();
    s.mismatches = c_mismatch.load();
    s.notCheckable = c_notCheckable.load();
    s.fastMs = static_cast<double>(g_fastTicks.load()) * g_qpcMs;
    s.checkedGameMs = static_cast<double>(g_checkGameTicks.load()) * g_qpcMs;
    s.checkedFastMs = static_cast<double>(g_checkFastTicks.load()) * g_qpcMs;
    s.selfDisabled = g_selfDisabled.load();
    std::lock_guard<std::mutex> lock(g_mismatchLock);
    s.lastMismatch = g_lastMismatch;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off: a check found a block that differs from the game's (see ApexRadiance_LOG.txt)";
    if (!s.images) return "On (no textures compressed yet)";
    std::string t = std::format("On: {} textures ({:.1f} M pixels) in {:.0f} ms; {} checked against the game, all equal", s.images, static_cast<double>(s.pixels) / 1e6, s.fastMs,
                                s.checked);
    if (s.checkedFastMs > 0.0 && s.checkedGameMs > 0.0) t += std::format(" ({:.1f}x faster on those)", s.checkedGameMs / s.checkedFastMs);
    return t;
}

void RenderDeveloperUI() {
    if constexpr (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Faster texture compression: " + StatusText()).c_str());
    ImGui::TextDisabled("Textures %llu, blocks %llu (flat-luma %llu, solid %llu), encoded by the game's own function %llu; passed through while off %llu",
                        static_cast<unsigned long long>(s.images), static_cast<unsigned long long>(s.blocks), static_cast<unsigned long long>(s.powerAxis),
                        static_cast<unsigned long long>(s.solid), static_cast<unsigned long long>(s.delegated), static_cast<unsigned long long>(s.passedThrough));
    ImGui::TextDisabled("Apex's encoder %.1f ms in total; checked textures: game %.1f ms, Apex %.1f ms%s", s.fastMs, s.checkedGameMs, s.checkedFastMs,
                        s.checkedFastMs > 0.0 ? std::format(" ({:.1f}x)", s.checkedGameMs / s.checkedFastMs).c_str() : "");
    ImGui::TextDisabled("CPU: %s", DxtCodec::CpuFeatureText());
    int every = g_verifyEvery.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Check 1 texture in N against the game##FdVerify", &every, 0, 64)) SetVerifyEvery(every);
    ImGui::SameLine();
    if (ImGui::SmallButton("Check every texture for 30 s##FdVerifyAll")) VerifyAllFor(30.0);
    const bool all = GetTickCount64() < g_verifyAllUntil.load();
    ImGui::TextDisabled("Checks: %llu equal, %llu different, %llu too large to check%s", static_cast<unsigned long long>(s.checked), static_cast<unsigned long long>(s.mismatches),
                        static_cast<unsigned long long>(s.notCheckable), all ? "  [checking every texture]" : "");
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
}

} // namespace FastDxt
