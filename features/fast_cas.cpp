#include "developer_settings.h"
// Faster Sim building: the CAS model builder's triangle sort answered by features/cas_tri_sort.h (see fast_cas.h and
// docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_cas.h"
#include "cas_tri_sort.h"
#include "dxt_codec.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <xmmintrin.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace FastCas {
namespace {

using EntryChain::Layer;
using EntryChain::Site;
// cdecl(u16* indices, u8* vertices, u32 indexCount, u32 vertexCount, u16 stride, u8 offset): every argument a 32-bit push
using SortFn = void(__cdecl*)(uint16_t*, const uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);

constexpr uint32_t kStartupChecks = 16;
constexpr uint64_t kParallelMinTests = 300000; // triangles x vertices below this: the calling thread alone (a wake costs more)
constexpr uint32_t kMaxWorkers = 6;
constexpr uint32_t kChunksPerThread = 4;
constexpr uint32_t kClosed = 0x80000000u, kCountMask = 0x7FFFFFFFu;
constexpr SIZE_T kWorkerStack = 256 * 1024;

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false}, g_selfDisabled{false};
std::atomic<uint32_t> g_seq{0};
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 16};
double g_qpcMs = 0.0;
std::atomic<uint64_t> c_calls{0}, c_tris{0}, c_tests{0}, c_passed{0}, c_parallel{0}, c_busy{0}, c_checked{0}, c_mismatch{0};
std::atomic<uint64_t> g_fastTicks{0}, g_checkGameTicks{0}, g_checkFastTicks{0}, g_maxTicks{0};
std::mutex g_mismatchLock;
std::string g_lastMismatch;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

// ---- the worker pool (the hand-off protocol of the texture encoder's pool, features/dxt_codec.cpp) ----
struct Job {
    const CasTriSort::Input* in = nullptr;
    const CasTriSort::Fast::Prepared* prepared = nullptr;
    uint32_t* counts = nullptr;
    uint32_t triangles = 0, chunk = 1, mxcsr = 0;
    std::atomic<uint32_t> next{0};
};

void RunJob(Job& j) {
    for (;;) {
        const uint32_t a = j.next.fetch_add(j.chunk, std::memory_order_relaxed);
        if (a >= j.triangles) break;
        CasTriSort::Fast::CountRange(*j.in, *j.prepared, a, j.triangles - a < j.chunk ? j.triangles : a + j.chunk, j.counts);
    }
}

struct Pool {
    std::atomic<bool> inUse{false};
    std::atomic<uint32_t> state{kClosed};
    std::atomic<uint32_t> created{0};
    std::mutex createLock;
    bool createFailed = false;
    HANDLE done = nullptr;
    struct Worker {
        HANDLE wake = nullptr;
        Pool* pool = nullptr;
    } workers[kMaxWorkers];
    Job job;

    Pool() { done = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

    static DWORD WINAPI WorkerMain(void* p) {
        Worker& w = *static_cast<Worker*>(p);
        Pool& pool = *w.pool;
        for (;;) {
            if (WaitForSingleObject(w.wake, INFINITE) != WAIT_OBJECT_0) {
                Sleep(10);
                continue;
            }
            const uint32_t s = pool.state.fetch_add(1, std::memory_order_acq_rel);
            if (!(s & kClosed)) {
                const unsigned own = _mm_getcsr();
                _mm_setcsr(pool.job.mxcsr & ~0x3Fu); // the caller's rounding, FTZ and DAZ: the same floats as on its thread
                RunJob(pool.job);
                _mm_setcsr(own);
            }
            if (pool.state.fetch_sub(1, std::memory_order_acq_rel) == (kClosed | 1u)) SetEvent(pool.done);
        }
    }

    uint32_t EnsureWorkers(uint32_t n) {
        if (n > kMaxWorkers) n = kMaxWorkers;
        uint32_t have = created.load(std::memory_order_acquire);
        if (have >= n) return n;
        std::lock_guard<std::mutex> lock(createLock);
        have = created.load(std::memory_order_relaxed);
        if (!done) createFailed = true;
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static const auto setDescription = reinterpret_cast<SetDescription>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        while (have < n && !createFailed) {
            Worker& w = workers[have];
            w.pool = this;
            w.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!w.wake) {
                createFailed = true;
                break;
            }
            const HANDLE h = CreateThread(nullptr, kWorkerStack, &Pool::WorkerMain, &w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!h) {
                CloseHandle(w.wake);
                w.wake = nullptr;
                createFailed = true;
                break;
            }
            if (setDescription) setDescription(h, L"Apex CAS sort worker");
            CloseHandle(h); // never joined
            have++;
            created.store(have, std::memory_order_release);
        }
        return have < n ? have : n;
    }
};

Pool& ThePool() {
    static Pool* const pool = new Pool(); // intentionally leaked: its threads sleep until the process ends
    return *pool;
}
std::atomic<uint32_t> g_workersMade{0};

// Per thread: the prepared positions and the counts (reused)
thread_local CasTriSort::Fast::Prepared t_prepared;
thread_local std::vector<uint32_t> t_counts;

// The Fast path on `in`, the sorted indices written to `out` (may be in.indices)
void RunFast(const CasTriSort::Input& in, uint16_t* out) {
    const uint32_t tris = CasTriSort::Triangles(in);
    CasTriSort::Fast::Prepare(in, t_prepared);
    t_counts.resize(tris);
    const uint64_t tests = static_cast<uint64_t>(tris) * in.vertexCount;
    bool done = false;
    const uint32_t want = DxtCodec::Parallel::DefaultWorkers();
    if (want && tests >= kParallelMinTests && tris >= 8) {
        Pool& p = ThePool();
        if (p.inUse.exchange(true, std::memory_order_acquire)) {
            c_busy.fetch_add(1, std::memory_order_relaxed);
        } else {
            uint32_t k = p.EnsureWorkers(want < kMaxWorkers ? want : kMaxWorkers);
            g_workersMade.store(p.created.load(std::memory_order_relaxed), std::memory_order_relaxed);
            if (k) {
                Job& j = p.job; // written while closed: late wakes leave without reading it
                j.in = &in;
                j.prepared = &t_prepared;
                j.counts = t_counts.data();
                j.triangles = tris;
                j.chunk = tris / ((k + 1) * kChunksPerThread) + 1;
                j.mxcsr = _mm_getcsr();
                j.next.store(0, std::memory_order_relaxed);
                p.state.fetch_and(~kClosed, std::memory_order_release); // open
                for (uint32_t i = 0; i < k; i++) SetEvent(p.workers[i].wake);
                RunJob(j); // the caller counts too; when it finds none left, every chunk is taken
                p.state.fetch_or(kClosed, std::memory_order_acq_rel);
                for (int spin = 0; (p.state.load(std::memory_order_acquire) & kCountMask) != 0; spin++) {
                    if (spin < 4000) _mm_pause();
                    else WaitForSingleObject(p.done, 50);
                }
                c_parallel.fetch_add(1, std::memory_order_relaxed);
                done = true;
            }
            p.inUse.store(false, std::memory_order_release);
        }
    }
    if (!done) CasTriSort::Fast::CountRange(in, t_prepared, 0, tris, t_counts.data());
    CasTriSort::Fast::Finish(in, t_counts.data(), out);
}

void NoteTicks(uint64_t dt) {
    g_fastTicks.fetch_add(dt, std::memory_order_relaxed);
    uint64_t m = g_maxTicks.load(std::memory_order_relaxed);
    while (dt > m && !g_maxTicks.compare_exchange_weak(m, dt, std::memory_order_relaxed)) {
    }
}

// The game's function (layer FastCas of the entry chain)
void __cdecl Hook_TriSort(uint16_t* indices, const uint8_t* vertices, uint32_t indexCount, uint32_t vertexCount, uint32_t stride, uint32_t offset) {
    const SortFn game = reinterpret_cast<SortFn>(EntryChain::Next(Site::CasTriSort, Layer::FastCas));
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed) || !indices || indexCount < 3) {
        c_passed.fetch_add(1, std::memory_order_relaxed);
        game(indices, vertices, indexCount, vertexCount, stride, offset);
        return;
    }
    CasTriSort::Input in;
    in.indices = indices;
    in.vertices = vertices;
    in.indexCount = indexCount;
    in.vertexCount = vertexCount;
    in.stride = stride & 0xFFFFu;       // movzx word [ebp+18h]
    in.positionOffset = offset & 0xFFu; // movzx byte [ebp+1Ch]
    const uint32_t tris = CasTriSort::Triangles(in);
    c_calls.fetch_add(1, std::memory_order_relaxed);
    c_tris.fetch_add(tris, std::memory_order_relaxed);
    c_tests.fetch_add(static_cast<uint64_t>(tris) * vertexCount, std::memory_order_relaxed);
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    bool check = seq < kStartupChecks;
    if (!check) {
        const int n = g_verifyEvery.load(std::memory_order_relaxed);
        check = n > 0 && seq % static_cast<uint32_t>(n) == 0;
    }
    if (!check) {
        const uint64_t t0 = Qpc();
        try {
            RunFast(in, indices); // the indices are written only at the very end, after every allocation
        } catch (...) {           // out of memory: the game's function on the untouched indices
            c_passed.fetch_add(1, std::memory_order_relaxed);
            game(indices, vertices, indexCount, vertexCount, stride, offset);
            return;
        }
        NoteTicks(Qpc() - t0);
        return;
    }
    // Checked: the game's function on a copy of the indices, then Apex's result in place, compared
    std::vector<uint16_t> gameResult;
    try {
        gameResult.assign(indices, indices + indexCount);
    } catch (...) {
        c_passed.fetch_add(1, std::memory_order_relaxed);
        game(indices, vertices, indexCount, vertexCount, stride, offset);
        return;
    }
    const uint64_t t0 = Qpc();
    game(gameResult.data(), vertices, indexCount, vertexCount, stride, offset);
    const uint64_t t1 = Qpc();
    try {
        RunFast(in, indices);
    } catch (...) { // out of memory: the game's result
        std::memcpy(indices, gameResult.data(), static_cast<size_t>(indexCount) * 2);
        return;
    }
    const uint64_t t2 = Qpc();
    NoteTicks(t2 - t1);
    if (std::memcmp(gameResult.data(), indices, static_cast<size_t>(indexCount) * 2) != 0) {
        uint32_t at = 0;
        while (at < indexCount && gameResult[at] == indices[at]) at++;
        std::memcpy(indices, gameResult.data(), static_cast<size_t>(indexCount) * 2); // the game's result stands
        const std::string msg = std::format("{} triangles, {} vertices, stride {}, offset {}: first different index at {} (game {}, Apex {})", tris, vertexCount,
                                            in.stride, in.positionOffset, at, at < indexCount ? gameResult[at] : 0, at < indexCount ? indices[at] : 0);
        {
            std::lock_guard<std::mutex> lock(g_mismatchLock);
            if (g_lastMismatch.empty()) LOG_ERROR("[FastCas] Result differs from the game's: " + msg + ". The game's result was used; the feature turns itself off for this session.");
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
    if (!GameAddr::GroupAvailable("FastCasSort", &missing)) return fail(GameAddr::NotAvailable(missing));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    std::string err;
    if (!EntryChain::Install(Site::CasTriSort, Layer::FastCas, reinterpret_cast<void*>(&Hook_TriSort), &err)) return fail("Could not hook the CAS triangle sort: " + err);
    g_selfDisabled.store(false);
    g_seq.store(0); // the startup checks run again after every start
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[FastCas] On: the CAS triangle sort {:#010x} answered by Apex (up to {} worker threads for large parts); the first {} calls are checked "
                         "against the game",
                         EntryChain::GameFunction(Site::CasTriSort), DxtCodec::Parallel::DefaultWorkers(), kStartupChecks));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release);
    const bool removed = EntryChain::Remove(Site::CasTriSort, Layer::FastCas);
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastCas] Off ({} calls, {} triangles, {} checked, {} different, {:.0f} ms){}", s.calls, s.triangles, s.checked, s.mismatches, s.fastMs,
                         removed ? "" : "; the entry could not be put back (the hook stays and passes every call through)"));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

Stats GetStats() {
    Stats s;
    s.calls = c_calls.load();
    s.triangles = c_tris.load();
    s.tests = c_tests.load();
    s.passedThrough = c_passed.load();
    s.parallel = c_parallel.load();
    s.busy = c_busy.load();
    s.checked = c_checked.load();
    s.mismatches = c_mismatch.load();
    s.fastMs = static_cast<double>(g_fastTicks.load()) * g_qpcMs;
    s.checkedGameMs = static_cast<double>(g_checkGameTicks.load()) * g_qpcMs;
    s.checkedFastMs = static_cast<double>(g_checkFastTicks.load()) * g_qpcMs;
    s.maxMs = static_cast<double>(g_maxTicks.load()) * g_qpcMs;
    s.workers = g_workersMade.load();
    s.selfDisabled = g_selfDisabled.load();
    std::lock_guard<std::mutex> lock(g_mismatchLock);
    s.lastMismatch = g_lastMismatch;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off: a result differed from the game's (see ApexRadiance_LOG.txt)";
    if (!s.calls) return "On (no Sim built yet)";
    std::string t = std::format("On: {} parts sorted ({} triangles) in {:.0f} ms, the slowest {:.1f} ms; {} checked against the game, all equal", s.calls, s.triangles, s.fastMs,
                                s.maxMs, s.checked);
    if (s.checked && s.checkedFastMs > 0.0) t += std::format(" (on those: the game {:.0f} ms, Apex {:.0f} ms)", s.checkedGameMs, s.checkedFastMs);
    return t;
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Faster Sim building: " + StatusText()).c_str());
    ImGui::TextDisabled("Calls %llu (passed to the game %llu), triangle x vertex tests %.1f M, on %u worker threads %llu (pool busy %llu)", static_cast<unsigned long long>(s.calls),
                        static_cast<unsigned long long>(s.passedThrough), static_cast<double>(s.tests) / 1e6, s.workers, static_cast<unsigned long long>(s.parallel),
                        static_cast<unsigned long long>(s.busy));
    int every = g_verifyEvery.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Check 1 call in N against the game (0 = only the first 16)##FcVerify", &every, 0, 64)) g_verifyEvery.store(every < 0 ? 0 : every);
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
}


void SaveDeveloperState(toml::table& out) {
    out.insert("verify_every", g_verifyEvery.load());
}
void LoadDeveloperState(const toml::table& t) {
    if (auto n = t["verify_every"].value<int64_t>()) { const int v = static_cast<int>(*n); g_verifyEvery.store(std::clamp(v, 0, 1024)); }
}
} // namespace FastCas
