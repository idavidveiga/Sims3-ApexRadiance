// Development build only: the public build (S3SS_PUBLIC) compiles none of this; frame_profiler.h then has empty inline
// versions of the interface.
#ifndef S3SS_PUBLIC
// Frame-hitch profiler (see frame_profiler.h).
//
// ---- Frames ----
// Frame boundary = the first D3D9 registry Present hook. It is registered at priority -1000 (Priority is an int enum and the
// registry sorts by its value), so it runs before every module's Priority::First Present hook, in particular before
// PostScene resets the camera matrix it captured this frame. Frame time = time between two boundaries.
// Clock: RDTSC when the CPU reports an invariant TSC (calibrated against QPC over 20 ms when first enabled and refined from
// a long QPC baseline while running), otherwise QPC. RDTSC keeps the per-draw cost at a few ns.
//
// ---- Timed game functions (Steam 1.67.2, TS3W.exe, no ASLR) ----
// Every pattern was checked against re\TS3W.exe: unique in .text and matching at the Steam address; for Detours targets
// the first instructions it relocates were decoded by hand and no branch in .text lands inside them. On Steam the pattern
// must match at the Steam address (call sites: and the CALL must reach the Steam callee); on other builds it must match
// exactly once in .text. A target whose bytes do not match (another module detoured or patched it, other build) is
// skipped and listed in Advanced > Hooks. Everything is attached at the first frame boundary after the profiler is
// turned on (on the render thread, after the startup patches) and removed when it is turned off.
//   0x00EC9F00 render frame        __stdcall(1), ret 4    83 EC 20 56 E8..    callers 0xECA059 / 0xECA2E6 / 0xECAAFC.
//              BeginFrame (0x611620: device vtable +0xA4 BeginScene), scene + UI, then 0x611760 -> 0x611680, then at
//              +0xBA the inactive-window limiter (the 9 bytes Smooth Patch replaces with its DelayAfterFramePresentation call).
//   0x00611680 end frame + present __thiscall(3), ret 0xC  83 EC 20 56 8B F1.. device vtable +0xA8 (EndScene), +0x44
//              (Present); only caller 0x611766 (0x611760, itself called from 0xEC63C4 / 0xEC9EBA / 0xEC9FB5).
//   0x00C6C290 lot LOD scoring     __thiscall(4), ret 0x10 55 8B EC 83 E4 F0 81 EC 84 08..  from WorldManager::Update
//              (0xC6D6E4). Arg 2 = the camera point it stores at WorldManager+0x3A0 (used here as the camera-motion signal).
//   0x00AC20E0 lot detail request  __thiscall(1), ret 4    53 8A 5C 24 08 56 8B F1 8A 86 C1 00 00 00 3A C3..
//              if lot+0xC1 != (char)arg and lot+0xC9 (bulldozing) == 0: stores the flag and posts AddLotObjectsToScene
//              (0xAC1130, arg 1) or the demotion (arg 0) through PostRemoteMethodCall (0xABE9C0). Counted as promotions /
//              demotions with exactly that condition. Callers 0xC6A0F9, 0xC6C7C3, 0xC6C7CE, 0xAE61A7.
//   0x00AEB2E0 lot renderer update __thiscall(1), ret 4    56 8B F1 E8..       callers 0xC7CEDF, 0xAEB3F9, 0xAEB41C.
//   0x00AEA680 lot load stages     __thiscall(0), ret     81 EC BC 00 00 00 53 56..  budget 20 ms (35 / 2000 in some
//              states); only caller 0xAEB306 (inside 0xAEB2E0). Timed at that CALL (E8 rel32 -> hook), not at the entry:
//              Smooth Streaming detours the entry and verifies its bytes before installing. Site pattern at 0xAEB2F8
//              "80 7E 1E 00 75 0D 80 7E 1F 00 75 07 8B CE E8 ?? ?? ?? ?? 8D 8E 70 03 00 00 E8" (unique), CALL at +14.
//   0x00AD9E30 lot LOD switch      __thiscall(1), ret 4    56 8B F1 8B 4E 28 80 79 1E 00..  "World/LotImpostor/
//              LODOverrideHook"; calls 0xAEB3F0 (0xAEB2E0 twice + 0xADBAD0). Caller 0xADAEBC.
//   0x00ADBAD0 lot lighting setup  __thiscall(0), ret     53 55 56 8B F1 8B 46 40.. (pattern extended to 47 bytes: three
//              sibling loops 0xADB851 / 0xADBBA0 / 0xADBC30 share the first 45). Calls 0x6A80E0 and 0x6A4180 per level.
//   0x006A80E0 room lighting       __thiscall(0), ret     83 EC 08 55 56 57 8B F9..  per room: 0x6C54E0 + 0x6A3EC0
//              (synchronous room lightmap solve). Only caller 0xADBB37.
//   0x00ADB8F0 lot lighting update __thiscall(0), ret     83 EC 1C 56 8B F1 80 7E 18 00..  the budgeted per-frame room
//              lighting of normal lot loading (budget FUN_00ADB120, solves through FUN_006A8BA0). Only caller 0xAE4D2A
//              (FUN_00AE4CB0, called at the end of 0xAEB2E0).
//   0x00C845C0 terrain update      __thiscall(2), ret 8    callers 0xC6AF5A, 0xC6D68F, 0xC8525F. Timed at the per-frame
//              CALL in WorldManager::Update, 0xC6D68F (same reason as above). Site pattern at 0xC6D682
//              "8B 44 24 0C 50 8D 4C 24 14 51 8B 4E 58 E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00" (unique), CALL at +13. Calls from
//              the other two callers are not timed; Night Terrain Relight's own calls from its Present hook count as
//              "Present hooks (mod)".
//   0x00E4A050 GC_try_to_collect   __cdecl(1)             83 3D ?? ?? ?? ?? 00 74 06 FF 15..  Boehm: lock, GC_init if
//              needed, 0xE49FB0 GC_try_to_collect_inner. Only caller 0xD819AA (MonoScriptHost::Simulate, simulation thread);
//              the thread that calls it is taken as the simulation thread.
//   0x00ABFAC0 Lot::UpdateObjectSceneNode __thiscall(3), ret 0xC (same pattern as LotStreamingOptimizations). Optional and
//              session-only (see "Not hooked by default" below).
// Not hooked on purpose:
//   - 0x006A3EC0 (room lightmap solve): LotEdgeLighting checks its entry bytes before installing and would refuse to
//     install while it is detoured; it is timed through its only caller 0x006A80E0 instead.
//   - Lot::AddLotObjectsToScene (0xAC1130): the Lot Streaming throttle overwrites its entry with a JMP.
//   - 0x00ABFAC0 by default: LotStreamingOptimizations finds it by a pattern that starts at the entry each time it
//     installs; while detoured that fails and the object throttle stays off. Opt-in, not saved, for measurements.
//   - WorldManager::Update (0xC6D570): detoured by LotStreamingOptimizations' map view blocker.
//   - The entries of 0xAEA680 / 0xC845C0: see above. Side effect of the terrain call-site redirect: Smooth Streaming, if
//     it installs while the profiler is on, sees another target at 0xC6D68F and skips the flush of its terrain queue
//     when it is later turned off (its own check, not fatal). Turn the profiler on after Smooth Streaming to avoid it.
//   - On non-Steam builds, GC Scheduler finds 0xC6C290 by a pattern from the entry; while the profiler is on it falls back
//     to its second camera source.
//
// ---- Attribution ----
// Every timed call pushes a frame on a per-thread stack; on return, the call's inclusive time is added to its parent's
// child time. Per category the profiler records:
//   - exclusive (self) time: the call's time minus the time of every timed call inside it. Exclusive times never overlap,
//     so on the render thread they add up to at most the frame time ("Unattributed" = the rest);
//   - inclusive time: counted only for the outermost call of that category on the stack (no double counting under
//     recursion); it includes nested timed calls (e.g. "Lot load stages" inclusive contains the room lighting inside it).
// Time is accumulated per thread (lock-free, owner-written relaxed atomics) and read at each frame boundary as deltas,
// bucketed by thread: render thread (the one calling Present), simulation thread (the one calling GC_try_to_collect)
// and other threads. Calls still open on the render thread are split at the boundary, so the render thread's time goes to
// the frame in which it was spent. Other threads' calls are attributed to the frame in which they return.
// Special splits on the render thread: 0x611680 counts as "EndScene + overlays" until the Present boundary and as
// "Present (driver)" after it; the part of 0x00EC9F00 after 0x611680 returns is "Frame limiter". So a frame interval
// holds the previous Present's wait, the limiter, the game's update and render and this frame's EndScene.
// CPU time = frame time - Present (driver) - Frame limiter.
//
// ---- D3D9 (through the D3D9Hooks registry only) ----
// Start hooks at priority -1000 and end hooks at +1000 bracket every other module's hooks: the time between them (per draw
// and per Present) is the mod's own D3D hook time. A module hook that returns Skip / Block ends the chain before the end
// hook; such an open dispatch is recognised by its DeviceContext address (the registry's frame has returned when a later
// push comes from the same or a shallower stack depth) and dropped without counting. The measurement includes the cost
// of this profiler's own two hooks (a few ns each). Counts: DrawIndexedPrimitive / DrawPrimitive (game draws vs draws
// inside EndScene: overlays, Picture pass), primitives, CreateTexture / CreateRenderTarget / CreateVertexShader /
// CreatePixelShader, and optionally SetTexture / Set*Shader / Set*ShaderConstantF / SetRenderTarget.
// The registry only calls hooks before the device method, so the duration of resource creation cannot be measured
// through it. Not exposed by the registry at all: CreateVertexBuffer / CreateIndexBuffer, Lock / Unlock, SetRenderState;
// DrawPrimitiveUP / DrawIndexedPrimitiveUP only through ExtraHooks' single observer slot, which Frame Capture owns, so not
// used. Per-hook-name timing needs the optional instrumentation in d3d9_hook_registry.cpp (frame_profiler.h).
//
// ---- Main loop, services, jobs, waits, I/O (engine study, Steam 1.67.2; same verification as above) ----
// The render thread is the main thread; its loop (0xECA960) runs app state 0xEC6C30, ServiceManager::Update 0x588E00 ->
// 0x59ED20 (every service's vtable +0x1C update), SceneCaptureManager 0x9DE140, Scene::BeginFrame 0x6EBB70, the render
// frame 0xEC9F00, Scene::EndFrame 0x6E8810 and the clock tick 0x5943F0.
//   0x0059ED20 / 0x0059ED70  ServiceManager main / simulation loops (thiscall(float,float), ret 8): replaced by an
//              identical C++ walk (the pattern is the whole 0x44-byte body) that times each service call, keyed by the
//              update function (names from the study for the known ones, else address + vtable).
//   0x00599720 ExecuteJob (thiscall(job), ret 4): every thread; on the render thread keyed by [job+0x10], or for remote
//              calls (job function 0x7D9840, bytes checked) by the method: vtable+0x10 of [job+0x14], or [obj+0x10] for
//              PostRemoteMethodCall objects (vtable 0x10650C4, checked at 0xABEA0A).
//   0x0059A220 WaitForJob (thiscall(job), ret 4): render thread, keyed by the job waited for.
//   0x004E16F0 Mutex::Lock, 0x004E2760 Semaphore::Wait (thiscall(timeout*), ret 4): every thread pays two clock reads;
//              render-thread calls that blocked > 0.1 ms are booked (as a leaf under the running timed call), keyed by the
//              caller's return address. Semaphore waits inside WaitForJob count as the job wait.
//   0x004DB850 FileStream::Read (ret 8), 0x004DB8E0 Flush (ret), 0x004EC010 RefPack read (stdcall(5), ret 0x14):
//              timed on the render thread; bytes read counted.
//   0x006EBB70 / 0x006E8810 / 0x009DE140 / 0x00EC6C30 / 0x005943F0 / 0x00AD97E0: timed (see kCats).
// The functions many threads call (the loops, ExecuteJob, WaitForJob, Mutex, Semaphore, file I/O) are not attached with
// Detours but by hand with all other threads suspended and checked (see AttachSafe).
//
// ---- Sampling (Advanced, off by default) ----
// For the time no timed function covers ("Unattributed"): a sampler thread pauses the render and/or simulation thread
// g_sampleHz times a second (default 2000; high-resolution waitable timer), records EIP and the TS3W return addresses
// found on the first 512 bytes of its stack, and the render thread assigns the samples to frame intervals. Per hitch:
// share of samples per code class (TS3W, DXVK/driver, system, Apex, other ASI, other), the top 8 code locations (TS3W
// function start guessed from the int3 padding, or the module), the top 8 TS3W call sites on the stack and the TS3W
// callers of samples in system code (waits, heap, I/O). Session tables compare hitch frames with other frames.
// Cost: each sample pauses the target for the SuspendThread / GetThreadContext / 512-byte copy / ResumeThread round trip
// (typically 5-30 us under WOW64, measured and shown in the UI), i.e. roughly 1-6% of the sampled thread at 2000 Hz, plus
// the same order of CPU on the sampler's core. The render thread's per-frame work is a table update per sample.
//
// ---- Output ----
// Render thread: rings (graph, averages, median window, the last 200 hitches), histogram for percentiles, totals.
// Hitches go to a single-producer / single-consumer queue; a writer thread appends them to ApexRadiance_Hitches.txt at most
// once per second (and on "Save report now"). Nothing is written or flushed on the render thread.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "frame_profiler.h"
#include "d3d9_hooks.h"
#include "post_scene.h"
#include "memory_patch.h"
#include "apex_config.h"
#include "apex_paths.h"
#include "apex_log.h"
#include "imgui.h"
#include "ui/widgets.h"
#include <toml++/toml.hpp>
#include <intrin.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kHookName = "FrameProfiler";
constexpr int kPrioStart = -1000; // below Priority::First: before every other module's hooks
constexpr int kPrioEnd = 1000;    // above Priority::Last: after every other module's hooks

// ---- categories ----
enum Cat : int {
    kRenderFrame,
    kEndScene,
    kPresentHooks,
    kPresentDriver,
    kFrameLimiter,
    kModD3DHooks,
    kLotLodScoring,
    kLotDetailRequest,
    kLotRendererUpdate,
    kLotLoadStages,
    kLotViewSwitch,
    kLotLightingInit,
    kRoomLighting,
    kLotLightingUpdate,
    kLotObjectBuild,
    kTerrainUpdate,
    kScriptGC,
    kSceneBeginFrame,
    kSceneEndFrame,
    kSceneCapture,
    kAppState,
    kClockTick,
    kImpostorPump,
    kService,
    kJob,
    kJobWait,
    kMutexWait,
    kSemWait,
    kFileRead,
    kFileFlush,
    kRefPackRead,
    kCatCount
};
constexpr int kFirstGameCat = kLotLodScoring;

struct CatInfo {
    const char* name;
    const char* hint;
};
const CatInfo kCats[kCatCount] = {
    {"Render frame (game)", "FUN_00EC9F00, the game's frame render: scene and UI submission. Self time only (end of frame, Present, limiter and the mod's D3D hooks are separate)."},
    {"EndScene + overlays", "FUN_00611680 up to Present: IDirect3DDevice9::EndScene, which also runs the Apex overlay (menu), the Picture pass and, when installed, official S3SS's overlay."},
    {"Present hooks (mod)", "The D3D9 hook registry's Present hooks of all Apex modules (includes this profiler's own per-frame bookkeeping)."},
    {"Present (driver)", "IDirect3DDevice9::Present itself (DXVK): submission, GPU back-pressure and vsync wait. High = GPU or vsync bound."},
    {"Frame limiter", "After Present in FUN_00EC9F00: the Smooth Patch frame limiter, or the game's own ~30 ms sleep while the window is inactive."},
    {"D3D hooks (mod)", "Time inside the D3D9 hook registry for draw calls: all Apex modules' per-draw hooks."},
    {"Lot LOD scoring", "FUN_00C6C290 (from WorldManager::Update): scores the lots around the camera and requests detailed view or demotion."},
    {"Lot detail request", "FUN_00AC20E0: sets a lot's detailed-view flag and posts Lot::AddLotObjectsToScene (run inline when posted from its own thread)."},
    {"Lot renderer update", "FUN_00AEB2E0: per-lot renderer update; drives the lot load state machine."},
    {"Lot load stages", "FUN_00AEA680: the lot load state machine, running load stages for up to a 20 ms budget per call."},
    {"Lot LOD switch", "FUN_00AD9E30 (World/LotImpostor/LODOverrideHook): finishes a lot's switch at once (FUN_00AEB3F0: two renderer updates + lighting)."},
    {"Lot lighting setup", "FUN_00ADBAD0: per lot level, the room lighting below and the lot light solvers (FUN_006A4180)."},
    {"Room lighting + solve", "FUN_006A80E0: for every room, the room light update (FUN_006C54E0) and the synchronous room lightmap solve (FUN_006A3EC0)."},
    {"Lot lighting update", "FUN_00ADB8F0 (lot renderer update -> FUN_00AE4CB0): the per-frame room lighting of a lot, time-boxed by FUN_00ADB120 (5-30 ms), solving rooms through FUN_006A8BA0."},
    {"Lot object scene nodes", "Lot::UpdateObjectSceneNode (FUN_00ABFAC0): one object's scene node (AddLotObjectsToScene, Lot Streaming throttle). Optional, see Advanced."},
    {"Terrain update", "FUN_00C845C0 (from WorldManager::Update): terrain update / rebuild (Terrain/WorldLeafTexture)."},
    {"Script GC", "GC_try_to_collect (FUN_00E4A050): the explicit collection in MonoScriptHost::Simulate, on the simulation thread."},
    {"Scene::BeginFrame", "FUN_006EBB70 (main loop): submits the scene jobs and hands the render queue over; can wait for the previous frame's job."},
    {"Scene::EndFrame", "FUN_006E8810 (main loop): waits for the scene jobs and pops the render queue."},
    {"Scene capture", "FUN_009DE140, SceneCaptureManager: off-screen thumbnail / photo captures and their read-back."},
    {"App state update", "FUN_00EC6C30 (main loop): application state machine."},
    {"Game clock tick", "FUN_005943F0 (main loop): game clock."},
    {"Lot impostor pump", "FUN_00AD97E0: builds a lot impostor by pumping the services until its job is done (no Present meanwhile)."},
    {"Services (self)", "Service updates (ServiceManager, FUN_0059ED20 / FUN_0059ED70): the services' own code, outside the timed functions inside them. Per service in Advanced."},
    {"Jobs (self)", "JobManager::ExecuteJob (FUN_00599720): jobs run on this thread, outside the timed functions inside them. Per job in Advanced."},
    {"Wait for job", "JobManager::WaitForJob (FUN_0059A220): blocked until another thread finishes a job (jobs it runs itself are counted as jobs)."},
    {"Mutex wait", "EA::Thread::Mutex::Lock (FUN_004E16F0) calls that blocked for more than 0.1 ms (render thread only)."},
    {"Semaphore wait", "EA::Thread::Semaphore::Wait (FUN_004E2760) calls that blocked for more than 0.1 ms (render thread only, outside WaitForJob)."},
    {"File read", "FileStream::Read (FUN_004DB850, ReadFile) on the render thread."},
    {"File flush", "FileStream::Flush (FUN_004DB8E0, FlushFileBuffers) on the render thread."},
    {"RefPack read", "RefPack stream read + decompression (FUN_004EC010) on the render thread."},
};

// ---- settings ----
std::atomic<bool> g_enabled{false};
std::atomic<float> g_mult{2.0f};    // hitch = frame > max(g_mult x median, g_floorMs)
std::atomic<float> g_floorMs{8.0f};
std::atomic<bool> g_countState{true};
std::atomic<bool> g_writeFile{true};
std::atomic<bool> g_regTiming{false};       // Advanced option
std::atomic<bool> g_regTimingActive{false}; // option && enabled, read by the registry instrumentation
std::atomic<bool> g_sampleRender{false};    // statistical sampler: render thread
std::atomic<bool> g_sampleSim{false};       // statistical sampler: simulation thread
std::atomic<int> g_sampleHz{2000};
bool g_objectBuildWanted = false;           // session only (see the header comment)
bool g_stateHooksActive = false;            // the state-call counters are registered

// ---- clock ----
bool g_useTsc = false; // decided once, before the first hook is attached, never changed afterwards
double g_qpcFreq = 1.0;
double g_msPerTick = 0.0;
uint64_t g_calQpc0 = 0, g_calTsc0 = 0;
uint64_t g_blockTicks = ~0ull; // set by InitClock
std::once_flag g_clockOnce;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

inline uint64_t Now() {
    if (g_useTsc) return __rdtsc();
    return Qpc();
}

void InitClock() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = static_cast<double>(f.QuadPart);
    double tickFreq = g_qpcFreq;
    int r[4] = {};
    __cpuid(r, static_cast<int>(0x80000000));
    bool invariant = false;
    if (static_cast<unsigned>(r[0]) >= 0x80000007u) {
        __cpuid(r, static_cast<int>(0x80000007));
        invariant = (r[3] & (1 << 8)) != 0;
    }
    if (invariant) {
        const uint64_t q0 = Qpc();
        const uint64_t t0 = __rdtsc();
        const uint64_t qEnd = q0 + static_cast<uint64_t>(g_qpcFreq / 50.0); // 20 ms
        uint64_t q1 = q0;
        while ((q1 = Qpc()) < qEnd) YieldProcessor();
        const uint64_t t1 = __rdtsc();
        const double f2 = static_cast<double>(t1 - t0) * g_qpcFreq / static_cast<double>(q1 - q0);
        if (f2 > 1e8) {
            tickFreq = f2;
            g_calQpc0 = q0;
            g_calTsc0 = t0;
            g_useTsc = true;
        }
    }
    g_msPerTick = 1000.0 / tickFreq;
    g_blockTicks = static_cast<uint64_t>(tickFreq / 10000.0); // 0.1 ms: a Mutex / Semaphore call slower than this blocked
}

// Render thread: refines the TSC rate from the long baseline taken at InitClock
void RefineClock() {
    if (!g_useTsc) return;
    const uint64_t q = Qpc();
    const uint64_t t = __rdtsc();
    if (q > g_calQpc0 + static_cast<uint64_t>(g_qpcFreq) && t > g_calTsc0)
        g_msPerTick = 1000.0 * static_cast<double>(q - g_calQpc0) / (g_qpcFreq * static_cast<double>(t - g_calTsc0));
}

// ---- per-thread accumulators and call stacks ----
constexpr int kMaxSlots = 128;
constexpr int kMaxDepth = 32;
constexpr uintptr_t kStaleMargin = 64; // bytes; see the D3D9 note in the header comment

enum FrameFlags : uint8_t { kOuter = 1, kSwitchAtSplit = 2, kDispatch = 4 };

struct Frame {
    uint64_t start;
    uint64_t child; // inclusive time of the timed calls made inside this one
    const void* sp; // an address in the pushing function's stack frame: identity and stack depth
    uint8_t cat;
    uint8_t flags;
};

struct alignas(64) ThreadSlot {
    std::atomic<DWORD> tid{0};
    // written by the owning thread only (load + store, no locked instruction), read at frame boundaries
    std::atomic<uint64_t> excl[kCatCount]{};
    std::atomic<uint64_t> incl[kCatCount]{};
    std::atomic<uint32_t> calls[kCatCount]{};
    // owner only
    Frame stack[kMaxDepth]{};
    int depth = 0;
    int open[kCatCount]{};
    uint32_t presentFnExits = 0;
    uint64_t lastPresentFnExit = 0;
};

ThreadSlot g_slots[kMaxSlots];
std::atomic<int> g_slotsUsed{0};
thread_local ThreadSlot* t_slot = nullptr;
thread_local bool t_noSlot = false;
std::atomic<DWORD> g_renderTid{0}; // render thread (written in the Present hook, read by the sampler too)
std::atomic<DWORD> g_simTid{0}; // the thread calling GC_try_to_collect

ThreadSlot* GetSlot() {
    if (ThreadSlot* s = t_slot) return s;
    if (t_noSlot) return nullptr;
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < kMaxSlots; i++) {
        DWORD expected = 0;
        if (g_slots[i].tid.compare_exchange_strong(expected, tid)) {
            int used = g_slotsUsed.load();
            while (used < i + 1 && !g_slotsUsed.compare_exchange_weak(used, i + 1)) {}
            t_slot = &g_slots[i];
            return t_slot;
        }
    }
    t_noSlot = true; // more than kMaxSlots threads ran timed code: this one is not timed
    return nullptr;
}

inline void Add64(std::atomic<uint64_t>& a, uint64_t v) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline void Add32(std::atomic<uint32_t>& a, uint32_t v) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

inline void Discard(ThreadSlot* s) {
    Frame& f = s->stack[--s->depth];
    s->open[f.cat]--;
}

// Drops registry dispatches left open by a hook that returned Skip / Block: their frame lies at the same or a
// shallower stack depth than sp (a live parent dispatch is always well above a nested push).
inline void CleanStale(ThreadSlot* s, const void* sp) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(sp);
    while (s->depth > 0) {
        const Frame& t = s->stack[s->depth - 1];
        if (!(t.flags & kDispatch) || reinterpret_cast<uintptr_t>(t.sp) >= p + kStaleMargin) break;
        Discard(s);
    }
}

inline int Push(ThreadSlot* s, int cat, const void* sp, uint8_t flags) {
    CleanStale(s, sp);
    if (s->depth >= kMaxDepth) return -1;
    Frame& f = s->stack[s->depth];
    f.cat = static_cast<uint8_t>(cat);
    f.sp = sp;
    f.child = 0;
    f.flags = static_cast<uint8_t>(flags | (s->open[cat] == 0 ? kOuter : 0));
    s->open[cat]++;
    f.start = Now();
    return s->depth++;
}

// carve: time at the end of the call that belongs to carveCat instead (the frame limiter inside FUN_00EC9F00)
// inclOut / selfOut (optional): the call's inclusive and self time, 0 when the frame was dropped
inline void Pop(ThreadSlot* s, int idx, const void* sp, uint64_t end, uint64_t carve, int carveCat, uint64_t* inclOut = nullptr, uint64_t* selfOut = nullptr) {
    if (inclOut) *inclOut = 0;
    if (selfOut) *selfOut = 0;
    if (idx >= s->depth || s->stack[idx].sp != sp) return; // dropped (stale cleanup or split bookkeeping)
    while (s->depth - 1 > idx) Discard(s);
    Frame& f = s->stack[idx];
    const uint64_t inclT = end > f.start ? end - f.start : 0;
    const uint64_t rest = inclT > f.child ? inclT - f.child : 0;
    if (carve > rest) carve = rest;
    Add64(s->excl[f.cat], rest - carve);
    if (carve) Add64(s->excl[carveCat], carve);
    if (f.flags & kOuter) Add64(s->incl[f.cat], inclT);
    Add32(s->calls[f.cat], 1);
    s->open[f.cat]--;
    s->depth = idx;
    if (idx > 0) s->stack[idx - 1].child += inclT;
    if (inclOut) *inclOut = inclT;
    if (selfOut) *selfOut = rest - carve;
}

// Render thread, at the frame boundary: books the time spent so far by every open call and restarts them at `now`
void Split(ThreadSlot* s, uint64_t now) {
    for (int i = 0; i < s->depth; i++) {
        const Frame& f = s->stack[i];
        const uint64_t segEnd = (i + 1 < s->depth) ? s->stack[i + 1].start : now;
        const uint64_t span = segEnd > f.start ? segEnd - f.start : 0;
        Add64(s->excl[f.cat], span > f.child ? span - f.child : 0);
        if (f.flags & kOuter) Add64(s->incl[f.cat], now > f.start ? now - f.start : 0);
    }
    for (int i = 0; i < s->depth; i++) {
        Frame& f = s->stack[i];
        f.start = now;
        f.child = 0;
        if (f.flags & kSwitchAtSplit) { // FUN_00611680: EndScene until Present, Present afterwards
            s->open[f.cat]--;
            f.cat = kPresentDriver;
            f.flags = static_cast<uint8_t>(f.flags & ~(kSwitchAtSplit | kOuter));
            if (s->open[kPresentDriver] == 0) f.flags |= kOuter;
            s->open[kPresentDriver]++;
        }
    }
}

struct Scope {
    ThreadSlot* s;
    int idx;
    explicit Scope(int cat, uint8_t flags = 0) : s(GetSlot()), idx(s ? Push(s, cat, this, flags) : -1) {}
    ~Scope() {
        if (idx >= 0) Pop(s, idx, this, Now(), 0, 0);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// ---- D3D9 counters: only touched inside registry hooks, which the registry runs under its own mutex ----
struct D3DCounts {
    uint32_t dip = 0, dp = 0, gameDraws = 0, endFrameDraws = 0;
    uint64_t prims = 0;
    uint32_t setTexture = 0, setShader = 0, shaderConst = 0, setRT = 0;
    uint32_t createTex = 0, createVS = 0, createPS = 0, createRT = 0;
};
D3DCounts g_d3d;

// ---- game-side counters ----
std::atomic<uint32_t> g_lotsPromoted{0}, g_lotsDemoted{0};
std::atomic<int> g_camPointMoved{0}, g_camPointSeen{0};
float g_camPoint[3] = {}; // written only by the thread calling FUN_00C6C290 (WorldManager::Update)
bool g_camPointValid = false;
float g_camVp[4][4] = {}; // render thread
bool g_camVpValid = false;

// ---- keyed tables: services, jobs, waits ----
inline bool IsRenderThread() {
    return __readfsdword(0x24) == g_renderTid.load(std::memory_order_relaxed); // TEB ClientId.UniqueThread
}

inline uint32_t KeyHash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7feb352dU;
    k ^= k >> 15;
    k *= 0x846ca68bU;
    k ^= k >> 16;
    return k;
}

// Open-addressing table of times per key (key 0 = empty). No allocation; single writer.
template <uint32_t N> struct TimeTable {
    uint32_t keys[N];
    uint64_t incl[N];  // ticks
    uint64_t self[N];  // ticks
    uint32_t calls[N];
    uint32_t aux[N];   // first non-zero aux seen: services = vtable, waits = kind
    uint32_t used;
    uint32_t lost;
    void Clear() {
        if (used || lost) std::memset(this, 0, sizeof *this);
    }
    int Find(uint32_t key) const {
        if (!key) key = 1;
        uint32_t i = KeyHash(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] == key) return static_cast<int>(i);
            if (!keys[i]) return -1;
        }
        return -1;
    }
    void Add(uint32_t key, uint64_t in, uint64_t se, uint32_t n, uint32_t auxv) {
        if (!key) key = 1;
        uint32_t i = KeyHash(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] != key) {
                if (keys[i]) continue;
                if (used * 10 >= N * 9) break;
                keys[i] = key;
                used++;
            }
            incl[i] += in;
            self[i] += se;
            calls[i] += n;
            if (!aux[i]) aux[i] = auxv;
            return;
        }
        lost++;
    }
    template <uint32_t M> void Merge(const TimeTable<M>& o) {
        for (uint32_t i = 0; i < M; i++)
            if (o.keys[i]) Add(o.keys[i], o.incl[i], o.self[i], o.calls[i], o.aux[i]);
    }
    // Indices of the maxN largest inclusive times, descending
    int Top(int* out, int maxN) const {
        int n = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (!keys[i]) continue;
            if (n == maxN && incl[i] <= incl[out[n - 1]]) continue;
            int pos = n < maxN ? n++ : maxN - 1;
            while (pos > 0 && incl[out[pos - 1]] < incl[i]) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos] = static_cast<int>(i);
        }
        return n;
    }
};

// Wait kinds (aux of the wait tables)
enum WaitKind : uint32_t { kWaitJob = 1, kWaitMutex = 2, kWaitSemaphore = 3 };
const char* WaitKindName(uint32_t k) {
    return k == kWaitJob ? "job" : (k == kWaitMutex ? "mutex" : (k == kWaitSemaphore ? "semaphore" : "?"));
}
constexpr uint32_t kRemoteCallBit = 0x80000000u; // job key: a remote call's method (TS3W addresses are below 0x02000000)

// Current frame (render thread only; merged into the session tables and cleared at the frame boundary)
TimeTable<64> g_fSvc;
TimeTable<256> g_fJob;
TimeTable<128> g_fWait;
uint64_t g_fReadBytes = 0;
// Session, [0] = other frames, [1] = hitch frames (render thread; reset by Clear)
TimeTable<256> g_aSvc[2];
TimeTable<1024> g_aJob[2];
TimeTable<512> g_aWait[2];
uint64_t g_aReadBytes[2] = {};

// Services updated on other threads (the simulation loop FUN_0059ED70): session totals, any thread may write
struct OtherSvcSlot {
    std::atomic<uint32_t> key{0};
    std::atomic<uint32_t> vtable{0};
    std::atomic<uint64_t> incl{0}, self{0};
    std::atomic<uint32_t> calls{0};
};
constexpr int kOtherSvcSlots = 64;
OtherSvcSlot g_otherSvc[kOtherSvcSlots];
uint64_t g_otherSvcBaseIncl[kOtherSvcSlots] = {}, g_otherSvcBaseSelf[kOtherSvcSlots] = {}; // at the last Clear (render thread)
uint32_t g_otherSvcBaseCalls[kOtherSvcSlots] = {};

void AddOtherService(uint32_t key, uint32_t vtable, uint64_t in, uint64_t se) {
    for (auto& slot : g_otherSvc) {
        uint32_t k = slot.key.load(std::memory_order_acquire);
        if (!k) {
            uint32_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, key)) {
                slot.vtable.store(vtable, std::memory_order_relaxed);
                k = key;
            } else {
                k = expected;
            }
        }
        if (k != key) continue;
        slot.incl.fetch_add(in, std::memory_order_relaxed);
        slot.self.fetch_add(se, std::memory_order_relaxed);
        slot.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// Service update functions (vtable +0x1C main / +0x20 simulation) named in the engine study (Steam 1.67.2); others show
// as their address and vtable.
struct NamedAddress {
    uint32_t addr;
    const char* name;
};
const NamedAddress kServiceNames[] = {
    {0x00588890, "MessageServer"},
    {0x00598660, "Input (message pump)"},
    {0x00599A10, "JobManager (main-thread jobs)"},
    {0x005F0E50, "CAS SimService"},
    {0x00608630, "CAS TextureCompositor"},
    {0x006E3620, "Scene service (SceneObjectManager)"},
    {0x0071E640, "Swarm (VFX)"},
    {0x00733D20, "ResourceChangeMonitor"},
    {0x00733D30, "ResourceChangeMonitor (sim)"},
    {0x007377F0, "ResourceSystem"},
    {0x007A08C0, "ShaderSystem"},
    {0x00A37E60, "Crossroads (AccountManager)"},
    {0x00B3A960, "ObjectDesigner"},
    {0x00C7E3C0, "WorldManager"},
    {0x00C7E300, "WorldManager (sim)"},
};

std::string ServiceName(uint32_t fn, uint32_t vtable) {
    if (g_gameVersion == GameVersion::Steam)
        for (const auto& n : kServiceNames)
            if (n.addr == fn) return n.name;
    return std::format("service {:08X} (vtable {:08X})", fn, vtable);
}

std::string JobName(uint32_t key) {
    if (key & kRemoteCallBit) return std::format("remote call -> {:08X}", key & ~kRemoteCallBit);
    return std::format("job {:08X}", key);
}

// Remote calls run as jobs whose function is FUN_007D9840(handle, RemoteCall*, phase); it calls the object's vtable +0x10.
// PostRemoteMethodCall objects (vtable 0x010650C4, built at 0x00ABEA0A) keep the native method at +0x10.
uint32_t g_remoteCallJobFn = 0;  // 0 = not verified (non-Steam): jobs are keyed by their function only
uint32_t g_remoteMethodVtable = 0;

// No C++ objects (SEH): the job's function, or for remote calls the method they will run
uint32_t JobKey(const uint8_t* job) {
    __try {
        const uint32_t fn = *reinterpret_cast<const uint32_t*>(job + 0x10);
        if (fn && fn == g_remoteCallJobFn) {
            const uint8_t* rc = *reinterpret_cast<const uint8_t* const*>(job + 0x14);
            if (rc) {
                const uint32_t vt = *reinterpret_cast<const uint32_t*>(rc);
                const uint32_t method = vt == g_remoteMethodVtable ? *reinterpret_cast<const uint32_t*>(rc + 0x10) : *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(vt) + 0x10);
                if (method) return method | kRemoteCallBit;
            }
        }
        return fn;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// A Mutex / Semaphore call that blocked on the render thread: booked as a leaf child of the running timed call
void RecordBlocked(int cat, uint32_t kind, uint64_t dt, uint32_t site) {
    ThreadSlot* s = GetSlot();
    if (!s) return;
    if (s->depth > 0 && s->stack[s->depth - 1].cat == kJobWait) return; // already counted as the job wait around it
    Add64(s->excl[cat], dt);
    if (s->open[cat] == 0) Add64(s->incl[cat], dt);
    Add32(s->calls[cat], 1);
    if (s->depth > 0) s->stack[s->depth - 1].child += dt;
    g_fWait.Add(site, dt, dt, 1, kind);
}

// ---- timed game functions ----
enum TargetId : int {
    T_RenderFrame,
    T_EndFramePresent,
    T_LotLodScoring,
    T_LotDetailRequest,
    T_LotRendererUpdate,
    T_LotLoadStages,
    T_LotViewSwitch,
    T_LotLightingInit,
    T_RoomLighting,
    T_LotLightingUpdate,
    T_TerrainUpdate,
    T_ScriptGC,
    T_LotObjectBuild,
    T_ServiceLoopMain,
    T_ServiceLoopSim,
    T_ExecuteJob,
    T_WaitForJob,
    T_MutexLock,
    T_SemaphoreWait,
    T_FileRead,
    T_FileFlush,
    T_RefPackRead,
    T_SceneBeginFrame,
    T_SceneEndFrame,
    T_SceneCapture,
    T_AppState,
    T_ClockTick,
    T_ImpostorPump,
    kTargetCount
};

void* g_orig[kTargetCount] = {}; // Detours trampolines (the target address while not attached)

using FnThis0 = uint64_t(__fastcall*)(void*, void*);
using FnThis1 = uint64_t(__fastcall*)(void*, void*, uint32_t);
using FnThis2 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t);
using FnThis3 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t);
using FnThis4 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
using FnCdecl1 = uint64_t(__cdecl*)(uint32_t);

template <int T, typename Fn> inline Fn Orig() {
    return reinterpret_cast<Fn>(g_orig[T]);
}

// The hooks are __fastcall(ecx, edx, stack args...): ABI-identical to the originals' __thiscall(ecx, stack args) with
// callee cleanup, ecx and edx passed through untouched. Return type uint64_t keeps EDX:EAX. Arguments are passed as raw
// dwords (floats included), never converted.

uint64_t __fastcall Hook_RenderFrame(void* self, void* edx, uint32_t a) {
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    int idx = -1;
    uint32_t exits = 0;
    if (s) {
        exits = s->presentFnExits;
        idx = Push(s, kRenderFrame, const_cast<char*>(&marker), 0);
    }
    const uint64_t r = Orig<T_RenderFrame, FnThis1>()(self, edx, a);
    if (idx >= 0) {
        const uint64_t end = Now();
        const uint64_t carve = (s->presentFnExits != exits && end > s->lastPresentFnExit) ? end - s->lastPresentFnExit : 0;
        Pop(s, idx, const_cast<char*>(&marker), end, carve, kFrameLimiter);
    }
    return r;
}

uint64_t __fastcall Hook_EndFramePresent(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c) {
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    const int idx = s ? Push(s, kEndScene, const_cast<char*>(&marker), kSwitchAtSplit) : -1;
    const uint64_t r = Orig<T_EndFramePresent, FnThis3>()(self, edx, a, b, c);
    if (s) {
        const uint64_t end = Now();
        if (idx >= 0) Pop(s, idx, const_cast<char*>(&marker), end, 0, 0);
        s->presentFnExits++;
        s->lastPresentFnExit = end;
    }
    return r;
}

// arg 2 points to the camera point (16 bytes) the function copies to WorldManager+0x3A0 right away
uint64_t __fastcall Hook_LotLodScoring(void* self, void* edx, uint32_t dt, uint32_t cameraPoint, uint32_t c, uint32_t d) {
    if (cameraPoint) {
        float p[3];
        std::memcpy(p, reinterpret_cast<const void*>(static_cast<uintptr_t>(cameraPoint)), sizeof p);
        if (g_camPointValid) {
            const float m = std::max({std::fabs(p[0] - g_camPoint[0]), std::fabs(p[1] - g_camPoint[1]), std::fabs(p[2] - g_camPoint[2])});
            if (m > 1e-3f) g_camPointMoved.store(1, std::memory_order_relaxed);
        }
        std::memcpy(g_camPoint, p, sizeof p);
        g_camPointValid = true;
        g_camPointSeen.store(1, std::memory_order_relaxed);
    }
    Scope sc(kLotLodScoring);
    return Orig<T_LotLodScoring, FnThis4>()(self, edx, dt, cameraPoint, c, d);
}

// Same condition as the function itself: the flag changes only when it differs and the lot is not being bulldozed
uint64_t __fastcall Hook_LotDetailRequest(void* self, void* edx, uint32_t want) {
    const uint8_t* lot = static_cast<const uint8_t*>(self);
    const uint8_t w = static_cast<uint8_t>(want & 0xFF);
    if (lot[0xC1] != w && lot[0xC9] == 0) (w ? g_lotsPromoted : g_lotsDemoted).fetch_add(1, std::memory_order_relaxed);
    Scope sc(kLotDetailRequest);
    return Orig<T_LotDetailRequest, FnThis1>()(self, edx, want);
}

uint64_t __fastcall Hook_LotRendererUpdate(void* self, void* edx, uint32_t a) {
    Scope sc(kLotRendererUpdate);
    return Orig<T_LotRendererUpdate, FnThis1>()(self, edx, a);
}

uint64_t __fastcall Hook_LotLoadStages(void* self, void* edx) {
    Scope sc(kLotLoadStages);
    return Orig<T_LotLoadStages, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_LotViewSwitch(void* self, void* edx, uint32_t a) {
    Scope sc(kLotViewSwitch);
    return Orig<T_LotViewSwitch, FnThis1>()(self, edx, a);
}

uint64_t __fastcall Hook_LotLightingInit(void* self, void* edx) {
    Scope sc(kLotLightingInit);
    return Orig<T_LotLightingInit, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_RoomLighting(void* self, void* edx) {
    Scope sc(kRoomLighting);
    return Orig<T_RoomLighting, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_LotLightingUpdate(void* self, void* edx) {
    Scope sc(kLotLightingUpdate);
    return Orig<T_LotLightingUpdate, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_TerrainUpdate(void* self, void* edx, uint32_t a, uint32_t b) {
    Scope sc(kTerrainUpdate);
    return Orig<T_TerrainUpdate, FnThis2>()(self, edx, a, b);
}

uint64_t __cdecl Hook_ScriptGC(uint32_t stopFunc) {
    g_simTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    Scope sc(kScriptGC);
    return Orig<T_ScriptGC, FnCdecl1>()(stopFunc);
}

uint64_t __fastcall Hook_LotObjectBuild(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c) {
    Scope sc(kLotObjectBuild);
    return Orig<T_LotObjectBuild, FnThis3>()(self, edx, a, b, c);
}

// ServiceManager update loops, replaced (the original body is never run while attached). Same walk as the game's code
// (FUN_0059ED20 / FUN_0059ED70, 0x44 bytes each, pattern = the whole body): if list+0xC != 0, for each node from [list]
// until the list head: if byte node+0x1A != 0 and (byte node+0x14 & flag), service = [node+8], call vtable[vOff](dt,
// realDt) (thiscall, ret 8); the next node is read after the call, as the original does. Each call is timed.
void RunServices(uint8_t* list, uint32_t vOff, uint8_t flag, uint32_t dt, uint32_t realDt) {
    using FnSvc = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t);
    if (*reinterpret_cast<const uint32_t*>(list + 0x0C) == 0) return;
    uint8_t* node = *reinterpret_cast<uint8_t**>(list);
    while (node != list) {
        if (node[0x1A] != 0 && (node[0x14] & flag)) {
            void* svc = *reinterpret_cast<void**>(node + 8);
            void** vt = *reinterpret_cast<void***>(svc);
            const FnSvc fn = reinterpret_cast<FnSvc>(vt[vOff / 4]);
            ThreadSlot* s = GetSlot();
            volatile char marker = 0;
            const int idx = s ? Push(s, kService, const_cast<char*>(&marker), 0) : -1;
            fn(svc, nullptr, dt, realDt);
            if (idx >= 0) {
                uint64_t in = 0, se = 0;
                Pop(s, idx, const_cast<char*>(&marker), Now(), 0, 0, &in, &se);
                const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
                const uint32_t vtable = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt));
                if (IsRenderThread()) g_fSvc.Add(key, in, se, 1, vtable);
                else AddOtherService(key, vtable, in, se);
            }
        }
        node = *reinterpret_cast<uint8_t**>(node);
    }
}

uint64_t __fastcall Hook_ServiceLoopMain(void* self, void* /*edx*/, uint32_t dt, uint32_t realDt) {
    RunServices(static_cast<uint8_t*>(self), 0x1C, 1, dt, realDt);
    return 0;
}

uint64_t __fastcall Hook_ServiceLoopSim(void* self, void* /*edx*/, uint32_t dt, uint32_t realDt) {
    RunServices(static_cast<uint8_t*>(self), 0x20, 2, dt, realDt);
    return 0;
}

// JobManager::ExecuteJob(job): runs [job+0x10](job+8, [job+0x14], 4). Timed on every thread (category), keyed on the
// render thread.
uint64_t __fastcall Hook_ExecuteJob(void* self, void* edx, uint32_t job) {
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    const int idx = s ? Push(s, kJob, const_cast<char*>(&marker), 0) : -1;
    const bool render = IsRenderThread();
    const uint32_t key = render ? JobKey(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(job))) : 0; // read before: the job may be freed
    const uint64_t r = Orig<T_ExecuteJob, FnThis1>()(self, edx, job);
    if (idx >= 0) {
        uint64_t in = 0, se = 0;
        Pop(s, idx, const_cast<char*>(&marker), Now(), 0, 0, &in, &se);
        if (render) g_fJob.Add(key, in, se, 1, 0);
    }
    return r;
}

// JobManager::WaitForJob(job): render thread only, keyed by the job waited for (jobs it runs itself are timed as jobs)
uint64_t __fastcall Hook_WaitForJob(void* self, void* edx, uint32_t job) {
    if (!IsRenderThread()) return Orig<T_WaitForJob, FnThis1>()(self, edx, job);
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    const int idx = s ? Push(s, kJobWait, const_cast<char*>(&marker), 0) : -1;
    const uint32_t key = JobKey(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(job)));
    const uint64_t r = Orig<T_WaitForJob, FnThis1>()(self, edx, job);
    if (idx >= 0) {
        uint64_t in = 0, se = 0;
        Pop(s, idx, const_cast<char*>(&marker), Now(), 0, 0, &in, &se);
        g_fWait.Add(key, se, se, 1, kWaitJob);
    }
    return r;
}

// Mutex::Lock / Semaphore::Wait: every thread goes through (two clock reads); only render-thread calls that blocked
// longer than 0.1 ms are recorded, keyed by the caller's return address
uint64_t __fastcall Hook_MutexLock(void* self, void* edx, uint32_t timeout) {
    const uint64_t t0 = Now();
    const uint64_t r = Orig<T_MutexLock, FnThis1>()(self, edx, timeout);
    const uint64_t dt = Now() - t0;
    if (dt > g_blockTicks && IsRenderThread()) RecordBlocked(kMutexWait, kWaitMutex, dt, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    return r;
}

uint64_t __fastcall Hook_SemaphoreWait(void* self, void* edx, uint32_t timeout) {
    const uint64_t t0 = Now();
    const uint64_t r = Orig<T_SemaphoreWait, FnThis1>()(self, edx, timeout);
    const uint64_t dt = Now() - t0;
    if (dt > g_blockTicks && IsRenderThread()) RecordBlocked(kSemWait, kWaitSemaphore, dt, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    return r;
}

// File I/O: timed on the render thread only
uint64_t __fastcall Hook_FileRead(void* self, void* edx, uint32_t buffer, uint32_t size) {
    if (!IsRenderThread()) return Orig<T_FileRead, FnThis2>()(self, edx, buffer, size);
    g_fReadBytes += size;
    Scope sc(kFileRead);
    return Orig<T_FileRead, FnThis2>()(self, edx, buffer, size);
}

uint64_t __fastcall Hook_FileFlush(void* self, void* edx) {
    if (!IsRenderThread()) return Orig<T_FileFlush, FnThis0>()(self, edx);
    Scope sc(kFileFlush);
    return Orig<T_FileFlush, FnThis0>()(self, edx);
}

using FnStd5 = uint64_t(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint64_t __stdcall Hook_RefPackRead(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    if (!IsRenderThread()) return Orig<T_RefPackRead, FnStd5>()(a, b, c, d, e);
    Scope sc(kRefPackRead);
    return Orig<T_RefPackRead, FnStd5>()(a, b, c, d, e);
}

uint64_t __fastcall Hook_SceneBeginFrame(void* self, void* edx) {
    Scope sc(kSceneBeginFrame);
    return Orig<T_SceneBeginFrame, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_SceneEndFrame(void* self, void* edx) {
    Scope sc(kSceneEndFrame);
    return Orig<T_SceneEndFrame, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_SceneCapture(void* self, void* edx, uint32_t a, uint32_t b) {
    Scope sc(kSceneCapture);
    return Orig<T_SceneCapture, FnThis2>()(self, edx, a, b);
}

uint64_t __fastcall Hook_AppState(void* self, void* edx) {
    Scope sc(kAppState);
    return Orig<T_AppState, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_ClockTick(void* self, void* edx) {
    Scope sc(kClockTick);
    return Orig<T_ClockTick, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_ImpostorPump(void* self, void* edx, uint32_t job) {
    Scope sc(kImpostorPump);
    return Orig<T_ImpostorPump, FnThis1>()(self, edx, job);
}

struct TargetInfo {
    const char* name;
    uintptr_t steam;     // Steam 1.67.2: where the pattern starts (the function entry, or the context of a CALL)
    const char* pattern; // on Steam it must match at `steam`; elsewhere exactly once in .text
    int callOffset;      // -1: Detours on the function entry; else offset of the CALL rel32 inside the pattern
    uintptr_t callee;    // call-site targets: the function the CALL reaches on Steam 1.67.2 (cross-check)
    void* hook;
    const char* thread;  // expected thread, for the report
    bool optional;
    int safeLen = 0;     // > 0: patched by hand with this many relocation-free prologue bytes while all other threads are
                         // suspended and checked (functions called from many threads); 0: Detours / call site
};

// Call-site targets: FUN_00AEA680 and FUN_00C845C0 are detoured by Smooth Streaming, which checks their entry bytes before
// it installs. Their per-frame CALL is redirected instead (tracked 5-byte write, restored byte for byte): Smooth Streaming
// still installs, and when it is on, the time measured includes its hook.
const TargetInfo kTargets[kTargetCount] = {
    {"Render frame (FUN_00EC9F00)", 0x00EC9F00, "83 EC 20 56 E8 ?? ?? ?? ?? 8B F0 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? D9 E8", -1, 0,
        reinterpret_cast<void*>(&Hook_RenderFrame), "render", false},
    {"End frame + Present (FUN_00611680)", 0x00611680, "83 EC 20 56 8B F1 8B 86 EC 00 00 00 85 C0 74 15 80 BE 8D 00 00 00 00 75 0C", -1, 0,
        reinterpret_cast<void*>(&Hook_EndFramePresent), "render", false},
    {"Lot LOD scoring (FUN_00C6C290)", 0x00C6C290, "55 8B EC 83 E4 F0 81 EC 84 08 00 00 A1 ?? ?? ?? ?? 53 8B D9 8B 4D 0C 0F 28 8B A0 03 00 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotLodScoring), "render (WorldManager::Update)", false},
    {"Lot detail request (FUN_00AC20E0)", 0x00AC20E0, "53 8A 5C 24 08 56 8B F1 8A 86 C1 00 00 00 3A C3 0F 84 ?? ?? ?? ?? 80 BE C9 00 00 00 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotDetailRequest), "render (WorldManager::Update)", false},
    {"Lot renderer update (FUN_00AEB2E0)", 0x00AEB2E0, "56 8B F1 E8 ?? ?? ?? ?? 80 7E 0C 00 0F 84 ?? ?? ?? ?? 83 7E 2C 00 74 ?? 80 7E 1E 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotRendererUpdate), "render (lot pass FUN_00C7CEA0)", false},
    {"Lot load stages (CALL at 0x00AEB306)", 0x00AEB2F8, "80 7E 1E 00 75 0D 80 7E 1F 00 75 07 8B CE E8 ?? ?? ?? ?? 8D 8E 70 03 00 00 E8", 14, 0x00AEA680,
        reinterpret_cast<void*>(&Hook_LotLoadStages), "render (lot pass FUN_00C7CEA0)", false},
    {"Lot LOD switch (FUN_00AD9E30)", 0x00AD9E30, "56 8B F1 8B 4E 28 80 79 1E 00 75 0F 8B 44 24 08 C7 40 0C 07 00 00 00 5E C2 04 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotViewSwitch), "lot impostor builder", false},
    {"Lot lighting setup (FUN_00ADBAD0)", 0x00ADBAD0,
        "53 55 56 8B F1 8B 46 40 2B 46 30 8B 4E 34 2B 4E 38 8B 56 2C 2B 56 24 C1 F8 02 C1 E0 06 C1 F9 02 03 C1 C1 FA 02 8D 6C 10 C0 33 DB 85 ED 57 7E 43", -1, 0,
        reinterpret_cast<void*>(&Hook_LotLightingInit), "lot impostor builder", false},
    {"Room lighting + solve (FUN_006A80E0)", 0x006A80E0, "83 EC 08 55 56 57 8B F9 80 BF 80 02 00 00 00 75 05 E8 ?? ?? ?? ?? 8B 87 38 02 00 00 8B 8F 34 02 00 00", -1, 0,
        reinterpret_cast<void*>(&Hook_RoomLighting), "lot impostor builder", false},
    {"Lot lighting update (FUN_00ADB8F0)", 0x00ADB8F0,
        "83 EC 1C 56 8B F1 80 7E 18 00 0F 84 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2F 05 ?? ?? ?? ?? 76 04 83 46 50 01 80 7E 4E 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotLightingUpdate), "render (lot pass FUN_00C7CEA0)", false},
    {"Terrain update (CALL at 0x00C6D68F)", 0x00C6D682, "8B 44 24 0C 50 8D 4C 24 14 51 8B 4E 58 E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00", 13, 0x00C845C0,
        reinterpret_cast<void*>(&Hook_TerrainUpdate), "render (WorldManager::Update)", false},
    {"GC_try_to_collect (FUN_00E4A050)", 0x00E4A050,
        "83 3D ?? ?? ?? ?? 00 74 06 FF 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 75 05 E8 ?? ?? ?? ?? 56 6A 00 6A 00 6A 00 6A 00 6A 00 6A 00 E8 ?? ?? ?? ?? 8B 44 24 20 50 E8",
        -1, 0, reinterpret_cast<void*>(&Hook_ScriptGC), "simulation", false},
    {"Lot::UpdateObjectSceneNode (FUN_00ABFAC0)", 0x00ABFAC0, "83 EC 0C 83 B9 64 03 00 00 00 89 4C 24 04 0F 84 ?? ?? ?? ?? 83 B9 08 04 00 00 01", -1, 0,
        reinterpret_cast<void*>(&Hook_LotObjectBuild), "AddLotObjectsToScene's thread", true},
    // Engine study targets (Steam 1.67.2). The two service loops are replaced: their patterns are the whole 0x44-byte body.
    {"ServiceManager main loop (FUN_0059ED20)", 0x0059ED20,
        "57 8B F9 8B 47 0C 85 C0 74 36 56 8B 37 3B F7 74 2E 80 7E 1A 00 74 22 F6 46 14 01 74 1C 8B 4E 08 D9 44 24 10 8B 11 8B 42 1C 83 EC 08 D9 5C 24 04 D9 44 24 14 D9 1C 24 FF D0 8B 36 3B F7 "
        "75 D2 5E 5F C2 08 00",
        -1, 0, reinterpret_cast<void*>(&Hook_ServiceLoopMain), "render (main loop)", false, 6},
    {"ServiceManager sim loop (FUN_0059ED70)", 0x0059ED70,
        "57 8B F9 8B 47 0C 85 C0 74 36 56 8B 37 3B F7 74 2E 80 7E 1A 00 74 22 F6 46 14 02 74 1C 8B 4E 08 D9 44 24 10 8B 11 8B 42 20 83 EC 08 D9 5C 24 04 D9 44 24 14 D9 1C 24 FF D0 8B 36 3B F7 "
        "75 D2 5E 5F C2 08 00",
        -1, 0, reinterpret_cast<void*>(&Hook_ServiceLoopSim), "simulation", false, 6},
    {"JobManager::ExecuteJob (FUN_00599720)", 0x00599720,
        "53 55 56 8B 74 24 10 33 ED 39 6E 10 57 8B D9 74 3E 8D 7B 08 8B CF C7 46 24 04 00 00 00 E8 ?? ?? ?? ?? 8B 46 14 8B 56 10 6A 04 50 8D 4E 08 51 FF D2", -1, 0,
        reinterpret_cast<void*>(&Hook_ExecuteJob), "any (keyed on render)", false, 7},
    {"JobManager::WaitForJob (FUN_0059A220)", 0x0059A220, "51 55 56 57 8B E9 E8 ?? ?? ?? ?? 8B 8D 88 00 00 00 8D 95 88 00 00 00 33 FF 3B CA 89 7C 24 0C 74 0B", -1, 0,
        reinterpret_cast<void*>(&Hook_WaitForJob), "render (others pass through)", false, 6},
    {"Mutex::Lock (FUN_004E16F0)", 0x004E16F0, "53 55 56 8B F1 80 7E 24 00 57 74 63 8B 6C 24 14 83 7D 00 FF 56 75 14 FF 15 ?? ?? ?? ?? 83 46 20 01", -1, 0,
        reinterpret_cast<void*>(&Hook_MutexLock), "any (blocked, render)", false, 5},
    {"Semaphore::Wait (FUN_004E2760)", 0x004E2760, "56 8B 74 24 08 8B 06 83 F8 FF 57 8B F9 74 19 85 C0 74 15 E8 ?? ?? ?? ?? 8B 0E 3B C8 76 06 2B C8", -1, 0,
        reinterpret_cast<void*>(&Hook_SemaphoreWait), "any (blocked, render)", false, 5},
    {"FileStream::Read (FUN_004DB850)", 0x004DB850, "56 8B F1 8B 46 04 83 F8 FF 74 30 8B 54 24 0C 6A 00 8D 4C 24 10 51 8B 4C 24 10 52 51 50 FF 15 ?? ?? ?? ??", -1, 0,
        reinterpret_cast<void*>(&Hook_FileRead), "any (timed on render)", false, 6},
    {"FileStream::Flush (FUN_004DB8E0)", 0x004DB8E0, "56 8B F1 8B 4E 04 32 C0 83 F9 FF 74 20 53 51 FF 15 ?? ?? ?? ?? 85 C0 0F 95 C3 84 DB 75 0C FF 15", -1, 0,
        reinterpret_cast<void*>(&Hook_FileFlush), "any (timed on render)", false, 6},
    {"RefPack stream read (FUN_004EC010)", 0x004EC010, "56 8B 74 24 08 66 8B 06 8A E8 8A CC 0F B7 C1 8B D0 81 E2 FF 1F 00 00 81 FA FB 10 00 00 75 42", -1, 0,
        reinterpret_cast<void*>(&Hook_RefPackRead), "any (timed on render)", false, 5},
    {"Scene::BeginFrame (FUN_006EBB70)", 0x006EBB70, "51 56 8B F1 80 BE 90 02 00 00 00 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 80 B8 8D 00 00 00 00 0F 85", -1, 0,
        reinterpret_cast<void*>(&Hook_SceneBeginFrame), "render (main loop)", false},
    {"Scene::EndFrame (FUN_006E8810)", 0x006E8810, "53 56 8B F1 E8 ?? ?? ?? ?? 32 DB 88 98 D0 05 00 00 38 9E 9D 02 00 00 0F 84 ?? ?? ?? ?? 38 9E 90", -1, 0,
        reinterpret_cast<void*>(&Hook_SceneEndFrame), "render (main loop)", false},
    {"SceneCaptureManager (FUN_009DE140)", 0x009DE140, "55 8B EC 83 E4 F0 81 EC 54 07 00 00 53 56 57 8B F9 8D 47 1C 39 00 0F 84 ?? ?? ?? ?? 83 7D 08 00", -1, 0,
        reinterpret_cast<void*>(&Hook_SceneCapture), "render (main loop)", false},
    {"App state (FUN_00EC6C30)", 0x00EC6C30, "56 8B F1 8B 4E 04 85 C9 75 04 39 0E 74 2F 8B C1 F7 D8 23 C1 8B 0E 85 C9 74 05 39 41 04 74 10 50", -1, 0,
        reinterpret_cast<void*>(&Hook_AppState), "render (main loop)", false},
    {"Game clock tick (FUN_005943F0)", 0x005943F0, "83 EC 10 55 56 57 8D 44 24 14 50 8B F1 FF 15 ?? ?? ?? ?? 8B 7C 24 14 8B 6C 24 18 8B CF 2B 4E 38", -1, 0,
        reinterpret_cast<void*>(&Hook_ClockTick), "render (main loop)", false},
    {"Lot impostor pump (FUN_00AD97E0)", 0x00AD97E0, "83 EC 08 80 79 25 00 0F 84 ?? ?? ?? ?? 53 55 56 8B 35 ?? ?? ?? ?? 57 6A 00 68 ?? ?? ?? ?? 8B CE", -1, 0,
        reinterpret_cast<void*>(&Hook_ImpostorPump), "render", false},
};
const int kTargetCat[kTargetCount] = {kRenderFrame, kEndScene, kLotLodScoring, kLotDetailRequest, kLotRendererUpdate, kLotLoadStages,
    kLotViewSwitch, kLotLightingInit, kRoomLighting, kLotLightingUpdate, kTerrainUpdate, kScriptGC, kLotObjectBuild,
    kService, kService, kJob, kJobWait, kMutexWait, kSemWait, kFileRead, kFileFlush, kRefPackRead, kSceneBeginFrame, kSceneEndFrame, kSceneCapture, kAppState,
    kClockTick, kImpostorPump};

struct TargetState {
    uintptr_t addr = 0; // the function entry, or the CALL instruction for call-site targets
    bool attached = false;
    std::vector<MemPatch::PatchLocation> patched; // call-site targets: the original 5 bytes
    uintptr_t scanned = 0; // non-Steam builds: where the unique scan found the pattern (scanned once)
    uint8_t orig8[8] = {};   // safeLen targets: the original first 8 bytes
    uint8_t* tramp = nullptr; // safeLen targets: copied prologue + JMP back (kept for the process lifetime)
    std::string how;
    std::string status = "Off";
};
TargetState g_targets[kTargetCount]; // guarded by g_ctrlMutex
std::mutex g_ctrlMutex;
std::string g_summary; // guarded by g_ctrlMutex
std::atomic<bool> g_attachPending{false}; // attach at the next frame boundary (render thread)

struct TextSection {
    uintptr_t begin = 0;
    size_t size = 0;
};

TextSection GetText() {
    static const TextSection sec = [] {
        TextSection t;
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
            if (std::strncmp(reinterpret_cast<const char*>(s->Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0) {
                t.begin = base + s->VirtualAddress;
                t.size = s->Misc.VirtualSize;
                break;
            }
        }
        return t;
    }();
    return sec;
}

size_t PatternLength(const char* p) {
    size_t n = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        n++;
        p += 2;
    }
    return n;
}

// The pattern matches exactly at addr (inside the game's .text)
bool MatchAt(uintptr_t addr, const char* pattern) {
    const TextSection t = GetText();
    const size_t n = PatternLength(pattern);
    if (!t.begin || n == 0 || addr < t.begin || addr + n > t.begin + t.size) return false;
    return MemPatch::ScanPattern(reinterpret_cast<BYTE*>(addr), n, pattern) == addr;
}

// Unique match of the pattern in .text, 0 when absent or ambiguous
uintptr_t ScanUnique(const char* pattern, uintptr_t* firstOut = nullptr) {
    const TextSection t = GetText();
    const size_t n = PatternLength(pattern);
    if (!t.begin || n == 0 || t.size < n) return 0;
    const uintptr_t a = MemPatch::ScanPattern(reinterpret_cast<BYTE*>(t.begin), t.size, pattern);
    if (firstOut) *firstOut = a;
    if (!a) return 0;
    const uintptr_t next = a + 1;
    const uintptr_t end = t.begin + t.size;
    if (end - next >= n && MemPatch::ScanPattern(reinterpret_cast<BYTE*>(next), end - next, pattern)) return 0;
    return a;
}

// The function entry, or the CALL instruction for call-site targets (resolved, else the Steam 1.67.2 one)
uintptr_t DisplayAddress(int i) {
    if (g_targets[i].addr) return g_targets[i].addr;
    return kTargets[i].steam + static_cast<uintptr_t>(std::max(0, kTargets[i].callOffset));
}

uintptr_t CallTarget(uintptr_t call) {
    int32_t rel;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

// Sets st.addr to the function entry (entry targets) or to the CALL instruction (call-site targets), or explains why not
void ResolveTarget(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    st.addr = 0;
    uintptr_t start = 0;
    if (g_gameVersion == GameVersion::Steam) {
        if (!MatchAt(ti.steam, ti.pattern)) {
            st.status = std::format("Skipped: bytes at {:#010x} do not match (detoured or patched by another module?)", ti.steam);
            return;
        }
        start = ti.steam;
        st.how = "pattern matches at the Steam 1.67.2 address";
    } else {
        if (st.scanned && MatchAt(st.scanned, ti.pattern)) {
            start = st.scanned;
        } else {
            uintptr_t first = 0;
            start = ScanUnique(ti.pattern, &first);
            if (!start) {
                st.status = first ? std::format("Skipped: pattern matches more than once (first at {:#010x})", first) : "Skipped: pattern not found (other game build, or patched by another module)";
                return;
            }
            st.scanned = start;
        }
        st.how = "unique pattern match";
    }
    if (ti.callOffset < 0) {
        st.addr = start;
        return;
    }
    const uintptr_t call = start + static_cast<uintptr_t>(ti.callOffset);
    if (*reinterpret_cast<const uint8_t*>(call) != 0xE8) {
        st.status = std::format("Skipped: no CALL at {:#010x}", call);
        return;
    }
    if (g_gameVersion == GameVersion::Steam && CallTarget(call) != ti.callee) {
        st.status = std::format("Skipped: the CALL at {:#010x} reaches {:#010x}, expected {:#010x} (redirected by another module?)", call, CallTarget(call), ti.callee);
        return;
    }
    st.addr = call;
}

// Call-site targets: CALL rel32 -> the hook, which calls the original callee through g_orig
bool AttachCallSite(int i) {
    TargetState& st = g_targets[i];
    const uintptr_t call = st.addr;
    const uintptr_t callee = CallTarget(call);
    std::vector<BYTE> orig(5), bytes(5);
    std::memcpy(orig.data(), reinterpret_cast<const void*>(call), 5);
    bytes[0] = 0xE8;
    const int32_t rel = MemPatch::CalculateRelativeOffset(call, reinterpret_cast<uintptr_t>(kTargets[i].hook));
    std::memcpy(&bytes[1], &rel, 4);
    g_orig[i] = reinterpret_cast<void*>(callee); // set before the CALL can reach the hook
    st.patched.clear();
    if (!MemPatch::WriteBytes(call, bytes, &st.patched, &orig)) {
        MemPatch::RestoreAll(st.patched);
        st.status = std::format("Skipped: could not patch the CALL at {:#010x}", call);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    st.attached = true;
    st.status = std::format("Timed at the CALL {:#010x} -> {:#010x} ({})", call, callee, st.how);
    return true;
}

// ---- hand-made hooks for functions many threads call (Mutex::Lock, ExecuteJob, file reads...) ----
// Detours only protects the threads passed to DetourUpdateThread, and DetourUpdateThread / the commit allocate from the
// heap, which is unsafe once other threads are suspended. Here everything is allocated first: the trampoline (the
// verified, relocation-free prologue bytes + JMP back), the thread handles. Then every other thread is suspended and its
// EIP checked: if one is inside the prologue being replaced, all are resumed and it is retried. The patch is one locked
// 8-byte write (the entry is 16-byte aligned), so no thread can fetch half of it. While threads are suspended only
// GetThreadContext, VirtualProtect, the locked write and FlushInstructionCache run. Trampolines are never freed, so a
// thread still inside one after the hook is removed stays valid.
uint8_t* g_trampPool = nullptr; // guarded by g_ctrlMutex
size_t g_trampUsed = 0;
constexpr size_t kTrampPool = 4096, kTrampSlot = 32;

uint8_t* BuildTrampoline(uintptr_t target, int len) {
    if (!g_trampPool) g_trampPool = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTrampPool, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampPool || g_trampUsed + kTrampSlot > kTrampPool) return nullptr;
    uint8_t* t = g_trampPool + g_trampUsed;
    g_trampUsed += kTrampSlot;
    std::memcpy(t, reinterpret_cast<const void*>(target), static_cast<size_t>(len));
    t[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>((target + len) - (reinterpret_cast<uintptr_t>(t) + len + 5));
    std::memcpy(t + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, kTrampSlot);
    return t;
}

bool WriteQwordAtomic(uintptr_t addr, const uint8_t bytes[8]) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    int64_t v;
    std::memcpy(&v, bytes, 8);
    volatile int64_t* p = reinterpret_cast<volatile int64_t*>(addr);
    int64_t cur = _InterlockedCompareExchange64(p, 0, 0); // atomic read (writes only if it was 0)
    for (;;) {
        const int64_t prev = _InterlockedCompareExchange64(p, v, cur);
        if (prev == cur) break;
        cur = prev;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 8);
    return true;
}

// Opens every other thread of the process (before anything is suspended)
std::vector<HANDLE> OpenOtherThreads() {
    std::vector<HANDLE> out;
    out.reserve(256);
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)) out.push_back(h);
    }
    CloseHandle(snap);
    return out;
}

bool AttachSafe(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    const uintptr_t addr = st.addr;
    const int len = ti.safeLen;
    if ((addr & 7) || len < 5 || len > 8) {
        st.status = std::format("Skipped: {:#010x} is not 8-byte aligned", addr);
        return false;
    }
    if (!st.tramp) st.tramp = BuildTrampoline(addr, len);
    if (!st.tramp) {
        st.status = "Skipped: no trampoline memory";
        return false;
    }
    std::memcpy(st.orig8, reinterpret_cast<const void*>(addr), 8);
    uint8_t patch[8];
    std::memcpy(patch, st.orig8, 8);
    patch[0] = 0xE9;
    const int32_t rel = MemPatch::CalculateRelativeOffset(addr, reinterpret_cast<uintptr_t>(ti.hook));
    std::memcpy(patch + 1, &rel, 4);
    g_orig[i] = st.tramp; // before the entry can reach the hook
    std::vector<HANDLE> threads = OpenOtherThreads();
    bool written = false;
    for (int attempt = 0; attempt < 100 && !written; attempt++) {
        // ---- other threads suspended: no allocation, no lock ----
        for (HANDLE h : threads) SuspendThread(h);
        bool busy = false;
        for (HANDLE h : threads) {
            CONTEXT ctx;
            std::memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Eip > addr && ctx.Eip < addr + static_cast<uintptr_t>(len)) {
                busy = true;
                break;
            }
        }
        if (!busy) written = WriteQwordAtomic(addr, patch);
        for (HANDLE h : threads) ResumeThread(h);
        // ---- resumed ----
        if (!busy) break;
        Sleep(1);
    }
    for (HANDLE h : threads) CloseHandle(h);
    if (!written) {
        g_orig[i] = reinterpret_cast<void*>(addr);
        st.status = "Skipped: a thread kept running its first instructions, or the write failed";
        return false;
    }
    st.attached = true;
    st.status = std::format("Timed ({}; hand-made hook, all threads checked)", st.how);
    return true;
}

// The JMP is a single instruction, so no thread can be inside it: the original bytes go back with one locked write
void DetachSafe(int i) {
    TargetState& st = g_targets[i];
    uint8_t now[8];
    std::memcpy(now, reinterpret_cast<const void*>(st.addr), 8);
    int32_t rel;
    std::memcpy(&rel, now + 1, 4);
    if (now[0] != 0xE9 || st.addr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel)) != reinterpret_cast<uintptr_t>(kTargets[i].hook)) {
        st.attached = false; // changed by someone else: leave it (the hook keeps forwarding through the trampoline)
        st.status = "Off (the entry was changed by another module after the profiler; left as it is)";
        return;
    }
    if (!WriteQwordAtomic(st.addr, st.orig8)) {
        st.status = "Restore failed: still timed";
        return;
    }
    st.attached = false;
    st.status = "Off";
}

bool AttachTarget(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    if (st.attached) return true;
    ResolveTarget(i);
    if (!st.addr) return false;
    if (ti.callOffset >= 0) return AttachCallSite(i);
    if (ti.safeLen > 0) {
        if (!MatchAt(st.addr, ti.pattern)) {
            st.status = std::format("Skipped: entry bytes at {:#010x} changed", st.addr);
            return false;
        }
        return AttachSafe(i);
    }
    if (!MatchAt(st.addr, ti.pattern)) {
        st.status = std::format("Skipped: entry bytes at {:#010x} changed", st.addr);
        return false;
    }
    g_orig[i] = reinterpret_cast<void*>(st.addr);
    if (DetourTransactionBegin() != NO_ERROR) {
        st.status = "Skipped: DetourTransactionBegin failed";
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    LONG err = DetourAttach(&g_orig[i], ti.hook);
    if (err != NO_ERROR) {
        DetourTransactionAbort();
        g_orig[i] = reinterpret_cast<void*>(st.addr);
        st.status = std::format("Skipped: DetourAttach failed ({})", err);
        return false;
    }
    err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        g_orig[i] = reinterpret_cast<void*>(st.addr);
        st.status = std::format("Skipped: DetourTransactionCommit failed ({})", err);
        return false;
    }
    st.attached = true;
    st.status = std::format("Timed ({})", st.how);
    return true;
}

void DetachTarget(int i) {
    TargetState& st = g_targets[i];
    if (!st.attached) return;
    if (kTargets[i].safeLen > 0) {
        DetachSafe(i);
        return;
    }
    if (kTargets[i].callOffset >= 0) { // g_orig keeps the callee, so a call already inside the hook still completes
        if (*reinterpret_cast<const uint8_t*>(st.addr) != 0xE8 || CallTarget(st.addr) != reinterpret_cast<uintptr_t>(kTargets[i].hook)) {
            // another module rewrote the CALL after us: leave its bytes alone (the hook keeps forwarding to the callee)
            st.patched.clear();
            st.attached = false;
            st.status = "Off (the CALL was changed by another module after the profiler; left as it is)";
            return;
        }
        if (!MemPatch::RestoreAll(st.patched)) {
            st.status = "Restore failed: still timed";
            LOG_ERROR(std::format("[FrameProfiler] Could not restore the CALL of {}", kTargets[i].name));
            return;
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(st.addr), 5);
        st.attached = false;
        st.status = "Off";
        return;
    }
    if (DetourTransactionBegin() != NO_ERROR) {
        st.status = "Detach failed: still timed";
        return;
    }
    DetourUpdateThread(GetCurrentThread());
    if (DetourDetach(&g_orig[i], kTargets[i].hook) != NO_ERROR) {
        DetourTransactionAbort();
        st.status = "Detach failed: still timed";
        LOG_ERROR(std::format("[FrameProfiler] Could not detach {}", kTargets[i].name));
        return;
    }
    if (DetourTransactionCommit() != NO_ERROR) {
        st.status = "Detach failed: still timed";
        LOG_ERROR(std::format("[FrameProfiler] Could not detach {}", kTargets[i].name));
        return;
    }
    st.attached = false;
    st.status = "Off";
}

// ---- sampling: code classes and the per-hitch summary ----
enum CodeClass : uint8_t { kClsGame, kClsGraphics, kClsSystem, kClsOurAsi, kClsOtherAsi, kClsOther, kClsCount };
const char* const kClassNames[kClsCount] = {"TS3W", "DXVK/driver", "system", "Apex", "other ASI", "other"};
constexpr int kTopN = 8;
constexpr int kTopWait = 4;

struct HitchSamples {
    uint16_t n[2] = {};                // samples of the render / simulation thread inside the frame interval
    uint16_t cls[2][kClsCount] = {};   // ... per code class
    uint32_t fnKey[2][kTopN] = {};     // hottest code: TS3W function start (heuristic) or module base
    uint16_t fnCount[2][kTopN] = {};
    uint8_t fnCls[2][kTopN] = {};
    uint32_t site[kTopN] = {};         // render thread: TS3W call sites (return addresses) found on the stack
    uint16_t siteCount[kTopN] = {};
    uint32_t wait[kTopWait] = {};      // render thread: first TS3W return address of samples in system code
    uint16_t waitCount[kTopWait] = {};
};

// Render thread, per hitch: the heaviest services, jobs and waits of the frame
constexpr int kTopSvc = 8, kTopJobs = 6, kTopWaits = 6;
struct HitchDetail {
    uint32_t svcKey[kTopSvc] = {}; // service update function
    uint32_t svcVt[kTopSvc] = {};  // its vtable
    float svcMs[kTopSvc] = {}, svcSelfMs[kTopSvc] = {};
    uint16_t svcCalls[kTopSvc] = {};
    uint32_t jobKey[kTopJobs] = {}; // job function, or kRemoteCallBit | remote call method
    float jobMs[kTopJobs] = {}, jobSelfMs[kTopJobs] = {};
    uint16_t jobCalls[kTopJobs] = {};
    uint32_t waitKey[kTopWaits] = {}; // job waited for, or the caller's return address for mutex / semaphore
    uint8_t waitKind[kTopWaits] = {};
    float waitMs[kTopWaits] = {};
    uint16_t waitCount[kTopWaits] = {};
    uint32_t readBytes = 0; // FileStream::Read on the render thread
};

// ---- per-frame record (also the hitch record) ----
struct HitchRecord {
    uint64_t frame = 0;
    double tSec = 0;
    float frameMs = 0, medianMs = 0, thresholdMs = 0;
    float cpuMs = 0, presentMs = 0, limiterMs = 0, modMs = 0, unattributedMs = 0;
    float render[kCatCount] = {};     // render thread, exclusive
    float renderIncl[kCatCount] = {}; // render thread, inclusive (outermost calls)
    float sim[kCatCount] = {};        // simulation thread, exclusive
    float other[kCatCount] = {};      // other threads, exclusive
    uint32_t calls[kCatCount] = {};   // all threads
    uint32_t gameDraws = 0, endFrameDraws = 0, dip = 0, dp = 0;
    uint64_t prims = 0;
    uint32_t setTexture = 0, setShader = 0, shaderConst = 0, setRT = 0;
    uint32_t createTex = 0, createVS = 0, createPS = 0, createRT = 0;
    uint32_t lotsPromoted = 0, lotsDemoted = 0;
    int8_t camera = -1; // 1 moving, 0 still, -1 unknown
    bool foreground = true;
    bool stateCounted = false;
    HitchSamples samples; // filled for hitches while the sampler runs
    HitchDetail detail;   // filled for hitches
};

// ---- render-thread statistics ----
constexpr int kMedianWindow = 120;
constexpr int kWarmupFrames = 30;
constexpr int kGraphFrames = 300;
constexpr int kLiveFrames = 60;
constexpr int kHitchRing = 200;
constexpr int kFineBins = 2000;  // 0.05 ms steps up to 100 ms
constexpr int kCoarseBins = 900; // 1 ms steps up to 1000 ms
constexpr int kBins = kFineBins + kCoarseBins + 1;

struct LiveSample {
    float frameMs, cpuMs, presentMs, limiterMs, modMs;
    float gameDraws, endFrameDraws, setTexture, setShader, shaderConst, setRT;
};

struct Stats {
    uint64_t frames = 0, hitches = 0;
    uint64_t framesMoving = 0, framesStill = 0, hitchesMoving = 0, hitchesStill = 0;
    double maxFrameMs = 0;
    std::array<uint32_t, kBins> hist{};
    double render[kCatCount] = {}, sim[kCatCount] = {}, other[kCatCount] = {};
    uint64_t bucketCalls[3][kCatCount] = {};
    double unattributed = 0;
    uint64_t lotsPromoted = 0, lotsDemoted = 0;
};

Stats g_stats;
float g_graph[kGraphFrames] = {};
int g_graphPos = 0, g_graphCount = 0;
LiveSample g_live[kLiveFrames] = {};
int g_livePos = 0, g_liveCount = 0;
float g_medianRing[kMedianWindow] = {};
int g_medianPos = 0, g_medianCount = 0;
HitchRecord g_hitches[kHitchRing];
int g_hitchPos = 0, g_hitchCount = 0;
HitchRecord g_cur;
uint32_t g_frameBucketCalls[3][kCatCount] = {};
float g_lastThreshold = 0, g_lastMedian = 0;
uint64_t g_frameIndex = 0, g_enableTicks = 0, g_lastBoundary = 0;
std::atomic<bool> g_needBaseline{true};

struct Seen {
    uint64_t excl[kCatCount];
    uint64_t incl[kCatCount];
    uint32_t calls[kCatCount];
};
Seen g_seen[kMaxSlots] = {};

// ---- hitch queue to the writer thread (single producer: render thread; single consumer: writer) ----
constexpr uint32_t kQueueSize = 256;
HitchRecord g_queue[kQueueSize];
std::atomic<uint32_t> g_qHead{0}, g_qTail{0};
std::atomic<uint32_t> g_qDropped{0}, g_qWritten{0};
std::atomic<bool> g_writerRunning{false};

// ---- optional per-hook registry timing (all access under the registry mutex, see frame_profiler.h) ----
struct RegName {
    std::string name;
    uint64_t ticks = 0;
    uint32_t calls = 0;
};
struct RegPtr {
    const char* ptr;
    int idx;
};
std::vector<RegName> g_regNames;
std::vector<RegPtr> g_regPtrs;
uint32_t g_regFrames = 0;
uint64_t g_regWindowStart = 0;
struct RegDisplay {
    std::string name;
    float msPerFrame;
    float callsPerFrame;
};
std::vector<RegDisplay> g_regDisplay; // render thread

// ---- statistical sampler ----
// A dedicated thread wakes about g_sampleHz times a second (high-resolution waitable timer), suspends the render and/or
// simulation thread, reads its context (EIP, ESP), copies up to 512 bytes of its stack and resumes it. Between
// SuspendThread and ResumeThread it only calls GetThreadContext and memcpy into a preallocated buffer: no heap, no
// logging, nothing that takes a lock the suspended thread could hold. The copy is bounded by the thread's stack region
// (VirtualQuery on the first ESP, done while the thread runs), so it cannot fault; it is SEH-guarded anyway.
// After ResumeThread the copy is scanned for dwords that point into TS3W's .text right after a CALL instruction
// (E8 rel32 reaching .text, or the FF /2 forms): candidate return addresses, a stack walk that needs no frame pointers
// (it can also pick up stale return addresses left in dead stack slots, so treat deep entries as hints). The samples go to
// a single-producer / single-consumer ring; at each frame boundary the render thread assigns them to the frame interval
// they were taken in (same clock as the frames).
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
constexpr int kMaxRets = 12;
constexpr size_t kStackCopy = 512;
constexpr uint32_t kSampleRing = 8192;
constexpr uint32_t kUnknownKey = 0xFFFFFFFFu;

struct Sample {
    uint64_t t;
    uint32_t eip;
    uint32_t ret[kMaxRets];
    uint8_t nRet;
    uint8_t thread; // 0 render, 1 simulation
};

Sample g_sampleRing[kSampleRing];
std::atomic<uint32_t> g_sHead{0}, g_sTail{0};
std::atomic<uint32_t> g_sDropped{0};
std::atomic<uint32_t> g_sTaken{0};       // written by the sampler thread only
std::atomic<uint64_t> g_sPausedTicks{0}; // time the targets spent suspended, sampler thread only
HANDLE g_samplerThread = nullptr;        // guarded by g_ctrlMutex
std::atomic<bool> g_samplerRunning{false};
std::atomic<bool> g_samplerStop{false};
uint32_t g_stackBuf[kStackCopy / 4];     // sampler thread only

// No C++ objects in here (SEH)
bool CopyStack(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// r points into .text (r >= begin + 7): is the instruction ending at r a CALL?
bool IsCallSite(uintptr_t r, uintptr_t begin, uintptr_t end) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(r);
    if (p[-5] == 0xE8) { // call rel32, target inside .text
        int32_t rel;
        std::memcpy(&rel, p - 4, 4);
        const uintptr_t target = r + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        if (target >= begin && target < end) return true;
    }
    if (p[-6] == 0xFF && (p[-5] == 0x15 || ((p[-5] & 0xF8) == 0x90 && p[-5] != 0x94))) return true; // call [disp32] / [reg+disp32]
    if (p[-7] == 0xFF && p[-6] == 0x94) return true;                                                  // call [sib+disp32]
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;                                                  // call [sib+disp8]
    if (p[-3] == 0xFF && (((p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) || p[-2] == 0x14)) return true;   // call [reg+disp8] / [sib]
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || ((p[-1] & 0xF8) == 0x10 && p[-1] != 0x14 && p[-1] != 0x15))) return true; // call reg / [reg]
    return false;
}

struct SampleTarget {
    DWORD tid = 0;
    HANDLE h = nullptr;
    uintptr_t stackLow = 0, stackEnd = 0; // the thread's stack reservation start and committed end
};

void Retarget(SampleTarget& tg, DWORD tid) {
    if (tg.h) CloseHandle(tg.h);
    tg = SampleTarget{};
    tg.tid = tid;
    if (tid && tid != GetCurrentThreadId()) tg.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
}

void TakeSample(SampleTarget& tg, uint8_t which, uintptr_t textBegin, uintptr_t textEnd) {
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    size_t copied = 0;
    const uint64_t t0 = Now();
    if (SuspendThread(tg.h) == static_cast<DWORD>(-1)) return;
    // ---- target suspended: GetThreadContext and memcpy only ----
    const bool ok = GetThreadContext(tg.h, &ctx) != FALSE;
    const uintptr_t esp = ctx.Esp;
    if (ok && tg.stackEnd && esp >= tg.stackLow && esp < tg.stackEnd) {
        copied = std::min<size_t>(kStackCopy, tg.stackEnd - esp) & ~static_cast<size_t>(3);
        if (!CopyStack(g_stackBuf, reinterpret_cast<const void*>(esp), copied)) copied = 0;
    }
    ResumeThread(tg.h);
    // ---- target running again ----
    const uint64_t t1 = Now();
    Add64(g_sPausedTicks, t1 - t0);
    if (!ok) return;
    if (!tg.stackEnd) { // first sample of this thread: its stack region bounds every later copy
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(esp), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
            tg.stackLow = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            tg.stackEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
    }
    const uint32_t h = g_sHead.load(std::memory_order_relaxed);
    if (h - g_sTail.load(std::memory_order_acquire) >= kSampleRing) {
        g_sDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Sample& s = g_sampleRing[h % kSampleRing];
    s.t = t0;
    s.eip = ctx.Eip;
    s.thread = which;
    s.nRet = 0;
    for (size_t k = 0; k < copied / 4 && s.nRet < kMaxRets; k++) {
        const uintptr_t v = g_stackBuf[k];
        if (v >= textBegin + 7 && v < textEnd && IsCallSite(v, textBegin, textEnd)) s.ret[s.nRet++] = static_cast<uint32_t>(v);
    }
    g_sHead.store(h + 1, std::memory_order_release);
    Add32(g_sTaken, 1);
}

DWORD WINAPI SamplerProc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); // older Windows: timer resolution
    const TextSection text = GetText();
    SampleTarget targets[2];
    while (!g_samplerStop.load(std::memory_order_relaxed)) {
        const int hz = std::clamp(g_sampleHz.load(std::memory_order_relaxed), 250, 4000);
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(10'000'000 / hz); // relative, 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
        else Sleep(1);
        for (uint8_t w = 0; w < 2; w++) {
            const bool want = w == 0 ? g_sampleRender.load(std::memory_order_relaxed) : g_sampleSim.load(std::memory_order_relaxed);
            const DWORD tid = want ? (w == 0 ? g_renderTid.load(std::memory_order_relaxed) : g_simTid.load(std::memory_order_relaxed)) : 0;
            if (tid != targets[w].tid) Retarget(targets[w], tid);
            if (targets[w].h) TakeSample(targets[w], w, text.begin, text.begin + text.size);
        }
    }
    for (auto& t : targets)
        if (t.h) CloseHandle(t.h);
    if (timer) CloseHandle(timer);
    return 0;
}

// Caller holds g_ctrlMutex
void UpdateSamplerLocked() {
    const bool want = g_enabled.load() && (g_sampleRender.load() || g_sampleSim.load());
    if (want && !g_samplerThread) {
        GetText(); // initialised here, not in the sampler
        g_samplerStop.store(false);
        g_samplerThread = CreateThread(nullptr, 0, SamplerProc, nullptr, 0, nullptr);
        if (!g_samplerThread) LOG_ERROR("[FrameProfiler] Could not start the sampler thread");
        else LOG_INFO("[FrameProfiler] Sampler on");
        g_samplerRunning.store(g_samplerThread != nullptr);
    } else if (!want && g_samplerThread) {
        g_samplerStop.store(true);
        WaitForSingleObject(g_samplerThread, 2000);
        CloseHandle(g_samplerThread);
        g_samplerThread = nullptr;
        LOG_INFO("[FrameProfiler] Sampler off");
        g_samplerRunning.store(false);
    }
}

// ---- module table (append-only: written by the render thread, read by the writer thread too) ----
struct ModuleEntry {
    uintptr_t base;
    uintptr_t end;
    uint8_t cls;
    char name[40]; // lower case
};
constexpr int kMaxModules = 384;
ModuleEntry g_modules[kMaxModules];
std::atomic<int> g_moduleCount{0};
uint64_t g_modulesRefreshTick = 0; // render thread

bool EndsWith(const char* s, const char* suffix) {
    const size_t a = std::strlen(s), b = std::strlen(suffix);
    return a >= b && std::strcmp(s + a - b, suffix) == 0;
}
bool StartsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

uint8_t ClassifyModule(HMODULE h, const char* n) {
    if (h == GetModuleHandleW(nullptr)) return kClsGame;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ClassifyModule), &self);
    if (h == self) return kClsOurAsi;
    if (EndsWith(n, ".asi")) return kClsOtherAsi;
    if (!std::strcmp(n, "d3d9.dll") || !std::strcmp(n, "dxgi.dll") || !std::strcmp(n, "d3d11.dll") || !std::strcmp(n, "vulkan-1.dll") || StartsWith(n, "nv") ||
        StartsWith(n, "amd") || StartsWith(n, "ati") || StartsWith(n, "igvk") || StartsWith(n, "igd"))
        return kClsGraphics;
    if (!std::strcmp(n, "ntdll.dll") || !std::strcmp(n, "kernelbase.dll") || !std::strcmp(n, "kernel32.dll") || !std::strcmp(n, "win32u.dll") ||
        !std::strcmp(n, "user32.dll") || !std::strcmp(n, "gdi32.dll") || !std::strcmp(n, "gdi32full.dll") || StartsWith(n, "wow64"))
        return kClsSystem;
    return kClsOther;
}

// Render thread only (the single writer)
void RefreshModules() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    const int count = std::min<int>(static_cast<int>(needed / sizeof(HMODULE)), 512);
    int n = g_moduleCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count && n < kMaxModules; i++) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mods[i]);
        bool known = false;
        for (int k = 0; k < n && !known; k++) known = g_modules[k].base == base;
        if (known) continue;
        MODULEINFO mi;
        wchar_t wname[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) || !GetModuleBaseNameW(GetCurrentProcess(), mods[i], wname, MAX_PATH)) continue;
        ModuleEntry& e = g_modules[n];
        e.base = base;
        e.end = base + mi.SizeOfImage;
        size_t j = 0;
        for (; wname[j] && j < sizeof(e.name) - 1; j++) {
            wchar_t c = wname[j];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
            e.name[j] = c < 128 ? static_cast<char>(c) : '?';
        }
        e.name[j] = 0;
        e.cls = ClassifyModule(mods[i], e.name);
        g_moduleCount.store(++n, std::memory_order_release);
    }
}

int FindModule(uintptr_t a) {
    const int n = g_moduleCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (a >= g_modules[i].base && a < g_modules[i].end) return i;
    return -1;
}

// Render thread: FindModule, refreshing the table (at most once a second) for addresses in modules not seen yet
int ModuleOf(uintptr_t a) {
    int i = FindModule(a);
    if (i < 0) {
        const uint64_t now = GetTickCount64();
        if (g_moduleCount.load() == 0 || now - g_modulesRefreshTick >= 1000) {
            g_modulesRefreshTick = now;
            RefreshModules();
            i = FindModule(a);
        }
    }
    return i;
}

uint8_t KeyClass(uint32_t key) {
    if (key == kUnknownKey) return kClsOther;
    const int mi = FindModule(key);
    return mi >= 0 ? g_modules[mi].cls : kClsOther;
}

// Code key as text: "TS3W fn~XXXXXXXX" (function start, heuristic) or the module name
std::string KeyText(uint32_t key) {
    if (key == kUnknownKey) return "(no module)";
    const int mi = FindModule(key);
    if (mi >= 0 && g_modules[mi].cls == kClsGame) return std::format("TS3W fn~{:08X}", key);
    if (mi >= 0) return g_modules[mi].name;
    return std::format("{:08X}", key);
}

inline uint32_t Hash32(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7feb352dU;
    k ^= k >> 15;
    k *= 0x846ca68bU;
    k ^= k >> 16;
    return k;
}

// Start of the TS3W function containing eip, by MSVC's layout: functions start 16-byte aligned after int3 (CC) padding.
// Checked on the 4126 functions of the RE dump: 94% start that way, 5.5% follow a RET without padding (those are merged
// with the function before them), and 0.1% of the aligned positions inside bodies follow a CC byte (split). A grouping
// key for the tables, not an exact symbol: resolve it with the decompile. Cached (direct-mapped).
constexpr uint32_t kFnCache = 16384;
uint32_t g_fnCacheKey[kFnCache] = {}, g_fnCacheVal[kFnCache] = {}; // render thread

uint32_t FnStartGuess(uint32_t eip) {
    const uint32_t slot = Hash32(eip) & (kFnCache - 1);
    if (g_fnCacheKey[slot] == eip && eip) return g_fnCacheVal[slot];
    const TextSection t = GetText();
    uint32_t result = eip & ~0xFu;
    if (eip >= t.begin + 16 && eip < t.begin + t.size) {
        uintptr_t p = eip & ~static_cast<uintptr_t>(0xF);
        for (int k = 0; k < 4096 && p > t.begin; k++, p -= 16) {
            if (*reinterpret_cast<const uint8_t*>(p - 1) == 0xCC) {
                result = static_cast<uint32_t>(p);
                break;
            }
        }
    }
    g_fnCacheKey[slot] = eip;
    g_fnCacheVal[slot] = result;
    return result;
}

// Open-addressing counter (key 0 = empty slot; key 0 is counted as 1). No allocation.
template <uint32_t N> struct CountTable {
    uint32_t keys[N];
    uint32_t counts[N];
    uint32_t used;
    uint32_t lost; // samples not counted because the table was full
    void Clear() { std::memset(this, 0, sizeof *this); }
    void Add(uint32_t key) {
        if (!key) key = 1;
        uint32_t i = Hash32(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] == key) {
                counts[i]++;
                return;
            }
            if (!keys[i]) {
                if (used * 10 >= N * 9) break;
                keys[i] = key;
                counts[i] = 1;
                used++;
                return;
            }
        }
        lost++;
    }
    uint32_t Get(uint32_t key) const {
        if (!key) key = 1;
        uint32_t i = Hash32(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] == key) return counts[i];
            if (!keys[i]) return 0;
        }
        return 0;
    }
    // The maxN largest counts, descending
    int Top(uint32_t* outKeys, uint32_t* outCounts, int maxN) const {
        int n = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (!keys[i]) continue;
            const uint32_t c = counts[i];
            if (n == maxN && c <= outCounts[n - 1]) continue;
            int pos = n < maxN ? n++ : maxN - 1;
            while (pos > 0 && outCounts[pos - 1] < c) {
                outKeys[pos] = outKeys[pos - 1];
                outCounts[pos] = outCounts[pos - 1];
                pos--;
            }
            outKeys[pos] = keys[i];
            outCounts[pos] = c;
        }
        return n;
    }
};

// Session aggregates, [0] = other frames, [1] = hitch frames (render thread; reset by Clear)
struct SampleAgg {
    uint32_t total[2];           // samples: [0] render thread, [1] simulation thread
    uint32_t cls[2][kClsCount];
    CountTable<4096> fn[2];      // hottest code per thread
    CountTable<4096> site;       // render thread: call sites on the stack
    CountTable<1024> wait;       // render thread: first TS3W return address of samples in system code
};
SampleAgg g_agg[2];
CountTable<4096> g_hotEip; // render thread, hitch frames: exact EIP

struct FrameSampleAgg {
    uint32_t n[2];
    uint32_t cls[2][kClsCount];
    CountTable<1024> fn[2];
    CountTable<1024> site;
    CountTable<256> wait;
};
FrameSampleAgg g_frameAgg; // scratch for the current hitch frame

void ClearSampleAggregates() {
    for (auto& a : g_agg) {
        std::memset(a.total, 0, sizeof a.total);
        std::memset(a.cls, 0, sizeof a.cls);
        a.fn[0].Clear();
        a.fn[1].Clear();
        a.site.Clear();
        a.wait.Clear();
    }
    g_hotEip.Clear();
}

// Render thread, at the frame boundary once the frame is classified: moves the samples taken in [intervalStart, now)
// into the session aggregates and, for a hitch, into out.
void ConsumeSamples(uint64_t intervalStart, uint64_t now, bool hitch, HitchSamples* out) {
    uint32_t t = g_sTail.load(std::memory_order_relaxed);
    const uint32_t h = g_sHead.load(std::memory_order_acquire);
    if (t == h) return;
    FrameSampleAgg& fa = g_frameAgg;
    if (hitch) {
        std::memset(fa.n, 0, sizeof fa.n);
        std::memset(fa.cls, 0, sizeof fa.cls);
        fa.fn[0].Clear();
        fa.fn[1].Clear();
        fa.site.Clear();
        fa.wait.Clear();
    }
    SampleAgg& agg = g_agg[hitch ? 1 : 0];
    for (; t != h; t++) {
        const Sample& s = g_sampleRing[t % kSampleRing];
        if (s.t >= now) break;             // belongs to the next frame
        if (s.t < intervalStart) continue; // left over from before this interval (sampler restarted, profiler re-enabled)
        const int thr = s.thread ? 1 : 0;
        const int mi = ModuleOf(s.eip);
        const uint8_t cls = mi >= 0 ? g_modules[mi].cls : kClsOther;
        const uint32_t key = cls == kClsGame ? FnStartGuess(s.eip) : (mi >= 0 ? static_cast<uint32_t>(g_modules[mi].base) : kUnknownKey);
        agg.total[thr]++;
        agg.cls[thr][cls]++;
        agg.fn[thr].Add(key);
        if (hitch) {
            fa.n[thr]++;
            fa.cls[thr][cls]++;
            fa.fn[thr].Add(key);
            if (thr == 0) g_hotEip.Add(s.eip);
        }
        if (thr == 0) {
            for (int k = 0; k < s.nRet; k++) {
                bool dup = false;
                for (int j = 0; j < k && !dup; j++) dup = s.ret[j] == s.ret[k];
                if (dup) continue;
                agg.site.Add(s.ret[k]);
                if (hitch) fa.site.Add(s.ret[k]);
            }
            if (cls == kClsSystem && s.nRet) {
                agg.wait.Add(s.ret[0]);
                if (hitch) fa.wait.Add(s.ret[0]);
            }
        }
    }
    g_sTail.store(t, std::memory_order_release);
    if (!hitch || !out) return;
    uint32_t keys[kTopN], counts[kTopN];
    for (int thr = 0; thr < 2; thr++) {
        out->n[thr] = static_cast<uint16_t>(std::min<uint32_t>(fa.n[thr], 65535));
        for (int c = 0; c < kClsCount; c++) out->cls[thr][c] = static_cast<uint16_t>(std::min<uint32_t>(fa.cls[thr][c], 65535));
        const int n = fa.fn[thr].Top(keys, counts, kTopN);
        for (int i = 0; i < n; i++) {
            out->fnKey[thr][i] = keys[i];
            out->fnCount[thr][i] = static_cast<uint16_t>(std::min<uint32_t>(counts[i], 65535));
            out->fnCls[thr][i] = KeyClass(keys[i]);
        }
    }
    const int ns = fa.site.Top(keys, counts, kTopN);
    for (int i = 0; i < ns; i++) {
        out->site[i] = keys[i];
        out->siteCount[i] = static_cast<uint16_t>(std::min<uint32_t>(counts[i], 65535));
    }
    const int nw = fa.wait.Top(keys, counts, kTopWait);
    for (int i = 0; i < nw; i++) {
        out->wait[i] = keys[i];
        out->waitCount[i] = static_cast<uint16_t>(std::min<uint32_t>(counts[i], 65535));
    }
}

void ClearFrameTables() {
    g_fSvc.Clear();
    g_fJob.Clear();
    g_fWait.Clear();
    g_fReadBytes = 0;
}

// Render thread, at the frame boundary once the frame is classified: the frame's services / jobs / waits go into the
// session tables and, for a hitch, the heaviest ones into out
void ConsumeFrameTables(bool hitch, HitchDetail* out) {
    const int b = hitch ? 1 : 0;
    g_aSvc[b].Merge(g_fSvc);
    g_aJob[b].Merge(g_fJob);
    g_aWait[b].Merge(g_fWait);
    g_aReadBytes[b] += g_fReadBytes;
    if (hitch && out) {
        const double k = g_msPerTick;
        int idx[8];
        int n = g_fSvc.Top(idx, kTopSvc);
        for (int i = 0; i < n; i++) {
            const int j = idx[i];
            out->svcKey[i] = g_fSvc.keys[j];
            out->svcVt[i] = g_fSvc.aux[j];
            out->svcMs[i] = static_cast<float>(static_cast<double>(g_fSvc.incl[j]) * k);
            out->svcSelfMs[i] = static_cast<float>(static_cast<double>(g_fSvc.self[j]) * k);
            out->svcCalls[i] = static_cast<uint16_t>(std::min<uint32_t>(g_fSvc.calls[j], 65535));
        }
        n = g_fJob.Top(idx, kTopJobs);
        for (int i = 0; i < n; i++) {
            const int j = idx[i];
            out->jobKey[i] = g_fJob.keys[j];
            out->jobMs[i] = static_cast<float>(static_cast<double>(g_fJob.incl[j]) * k);
            out->jobSelfMs[i] = static_cast<float>(static_cast<double>(g_fJob.self[j]) * k);
            out->jobCalls[i] = static_cast<uint16_t>(std::min<uint32_t>(g_fJob.calls[j], 65535));
        }
        n = g_fWait.Top(idx, kTopWaits);
        for (int i = 0; i < n; i++) {
            const int j = idx[i];
            out->waitKey[i] = g_fWait.keys[j];
            out->waitKind[i] = static_cast<uint8_t>(g_fWait.aux[j]);
            out->waitMs[i] = static_cast<float>(static_cast<double>(g_fWait.incl[j]) * k);
            out->waitCount[i] = static_cast<uint16_t>(std::min<uint32_t>(g_fWait.calls[j], 65535));
        }
        out->readBytes = static_cast<uint32_t>(std::min<uint64_t>(g_fReadBytes, 0xFFFFFFFFull));
    }
    ClearFrameTables();
}

// ---- frame boundary ----
int BinOf(float ms) {
    if (!(ms > 0.0f)) return 0;
    if (ms < 100.0f) return std::min(kFineBins - 1, static_cast<int>(ms / 0.05f));
    if (ms < 1000.0f) return kFineBins + std::min(kCoarseBins - 1, static_cast<int>(ms - 100.0f));
    return kBins - 1;
}

float BinValue(int b) {
    if (b < kFineBins) return (static_cast<float>(b) + 0.5f) * 0.05f;
    if (b < kFineBins + kCoarseBins) return 100.0f + static_cast<float>(b - kFineBins) + 0.5f;
    return 1000.0f;
}

float Percentile(double p) {
    if (!g_stats.frames) return 0.0f;
    const uint64_t target = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(p * static_cast<double>(g_stats.frames))));
    uint64_t cum = 0;
    for (int b = 0; b < kBins; b++) {
        cum += g_stats.hist[b];
        if (cum >= target) return BinValue(b);
    }
    return BinValue(kBins - 1);
}

float MedianOfWindow() {
    if (!g_medianCount) return 0.0f;
    float tmp[kMedianWindow];
    std::memcpy(tmp, g_medianRing, sizeof(float) * g_medianCount);
    const int mid = g_medianCount / 2;
    std::nth_element(tmp, tmp + mid, tmp + g_medianCount);
    return tmp[mid];
}

// r == nullptr: baseline only (forget what accumulated while the profiler was off)
void SnapshotThreads(HitchRecord* r) {
    const int used = g_slotsUsed.load(std::memory_order_acquire);
    const DWORD renderTid = g_renderTid.load(std::memory_order_relaxed);
    const DWORD simTid = g_simTid.load(std::memory_order_relaxed);
    if (r) std::memset(g_frameBucketCalls, 0, sizeof g_frameBucketCalls);
    for (int i = 0; i < used; i++) {
        ThreadSlot& s = g_slots[i];
        const DWORD tid = s.tid.load(std::memory_order_relaxed);
        Seen& seen = g_seen[i];
        const int bucket = tid == renderTid ? 0 : (simTid != 0 && tid == simTid ? 1 : 2);
        for (int c = 0; c < kCatCount; c++) {
            const uint64_t e = s.excl[c].load(std::memory_order_relaxed);
            const uint64_t n = s.incl[c].load(std::memory_order_relaxed);
            const uint32_t k = s.calls[c].load(std::memory_order_relaxed);
            if (r) {
                const float em = static_cast<float>(static_cast<double>(e - seen.excl[c]) * g_msPerTick);
                if (bucket == 0) {
                    r->render[c] += em;
                    r->renderIncl[c] += static_cast<float>(static_cast<double>(n - seen.incl[c]) * g_msPerTick);
                } else if (bucket == 1) {
                    r->sim[c] += em;
                } else {
                    r->other[c] += em;
                }
                r->calls[c] += k - seen.calls[c];
                g_frameBucketCalls[bucket][c] += k - seen.calls[c];
            }
            seen.excl[c] = e;
            seen.incl[c] = n;
            seen.calls[c] = k;
        }
    }
}

int8_t SampleCamera() {
    bool seen = false, moved = false;
    const int pointMoved = g_camPointMoved.exchange(0, std::memory_order_relaxed);
    if (g_camPointSeen.exchange(0, std::memory_order_relaxed)) {
        seen = true;
        moved = pointMoved != 0;
    }
    // (The combined build also used PostScene's camera view-projection as a fallback here; the standalone keeps
    // v0.1.0's PostScene, which does not track the camera, so frames without the lot LOD call count as "unknown".)
    return moved ? 1 : (seen ? 0 : -1);
}

bool IsForeground() {
    const HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void QueuePush(const HitchRecord& r) {
    const uint32_t h = g_qHead.load(std::memory_order_relaxed);
    if (h - g_qTail.load(std::memory_order_acquire) >= kQueueSize) {
        g_qDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_queue[h % kQueueSize] = r;
    g_qHead.store(h + 1, std::memory_order_release);
}

void RollRegistryWindow(uint64_t now) {
    g_regFrames++;
    if (static_cast<double>(now - g_regWindowStart) * g_msPerTick < 1000.0) return;
    g_regDisplay.clear();
    for (auto& n : g_regNames) {
        if (n.calls && g_regFrames) {
            g_regDisplay.push_back({n.name, static_cast<float>(static_cast<double>(n.ticks) * g_msPerTick / g_regFrames), static_cast<float>(n.calls) / g_regFrames});
        }
        n.ticks = 0;
        n.calls = 0;
    }
    std::sort(g_regDisplay.begin(), g_regDisplay.end(), [](const RegDisplay& a, const RegDisplay& b) { return a.msPerFrame > b.msPerFrame; });
    g_regFrames = 0;
    g_regWindowStart = now;
}

void UpdateStats(const HitchRecord& r, bool hitch) {
    Stats& s = g_stats;
    s.frames++;
    s.maxFrameMs = std::max(s.maxFrameMs, static_cast<double>(r.frameMs));
    s.hist[BinOf(r.frameMs)]++;
    for (int c = 0; c < kCatCount; c++) {
        s.render[c] += r.render[c];
        s.sim[c] += r.sim[c];
        s.other[c] += r.other[c];
        for (int b = 0; b < 3; b++) s.bucketCalls[b][c] += g_frameBucketCalls[b][c];
    }
    s.unattributed += r.unattributedMs;
    s.lotsPromoted += r.lotsPromoted;
    s.lotsDemoted += r.lotsDemoted;
    if (r.camera == 1) s.framesMoving++;
    if (r.camera == 0) s.framesStill++;
    if (hitch) {
        s.hitches++;
        if (r.camera == 1) s.hitchesMoving++;
        if (r.camera == 0) s.hitchesStill++;
    }
    g_graph[g_graphPos] = r.frameMs;
    g_graphPos = (g_graphPos + 1) % kGraphFrames;
    g_graphCount = std::min(g_graphCount + 1, kGraphFrames);
    LiveSample& l = g_live[g_livePos];
    l = {r.frameMs, r.cpuMs, r.presentMs, r.limiterMs, r.modMs, static_cast<float>(r.gameDraws), static_cast<float>(r.endFrameDraws), static_cast<float>(r.setTexture),
        static_cast<float>(r.setShader), static_cast<float>(r.shaderConst), static_cast<float>(r.setRT)};
    g_livePos = (g_livePos + 1) % kLiveFrames;
    g_liveCount = std::min(g_liveCount + 1, kLiveFrames);
}

void FrameBoundary(uint64_t now) {
    if (g_needBaseline.exchange(false)) {
        SnapshotThreads(nullptr);
        g_d3d = D3DCounts{};
        g_lotsPromoted.store(0);
        g_lotsDemoted.store(0);
        g_camPointMoved.store(0);
        g_camPointSeen.store(0);
        g_camVpValid = false;
        g_lastBoundary = now;
        g_enableTicks = now;
        g_regWindowStart = now;
        g_regFrames = 0;
        g_medianCount = g_medianPos = 0; // warm-up again: the first frames include attaching the hooks
        ClearFrameTables();
        return;
    }
    if ((g_frameIndex & 127) == 0) RefineClock();

    HitchRecord& r = g_cur;
    r = HitchRecord{};
    const uint64_t intervalStart = g_lastBoundary;
    r.frameMs = static_cast<float>(static_cast<double>(now - g_lastBoundary) * g_msPerTick);
    g_lastBoundary = now;
    r.frame = ++g_frameIndex;
    r.tSec = static_cast<double>(now - g_enableTicks) * g_msPerTick / 1000.0;
    SnapshotThreads(&r);

    r.gameDraws = g_d3d.gameDraws;
    r.endFrameDraws = g_d3d.endFrameDraws;
    r.dip = g_d3d.dip;
    r.dp = g_d3d.dp;
    r.prims = g_d3d.prims;
    r.setTexture = g_d3d.setTexture;
    r.setShader = g_d3d.setShader;
    r.shaderConst = g_d3d.shaderConst;
    r.setRT = g_d3d.setRT;
    r.createTex = g_d3d.createTex;
    r.createVS = g_d3d.createVS;
    r.createPS = g_d3d.createPS;
    r.createRT = g_d3d.createRT;
    r.stateCounted = g_stateHooksActive;
    g_d3d = D3DCounts{};
    r.lotsPromoted = g_lotsPromoted.exchange(0, std::memory_order_relaxed);
    r.lotsDemoted = g_lotsDemoted.exchange(0, std::memory_order_relaxed);
    r.camera = SampleCamera();

    r.presentMs = r.render[kPresentDriver];
    r.limiterMs = r.render[kFrameLimiter];
    r.modMs = r.render[kModD3DHooks] + r.render[kPresentHooks];
    r.cpuMs = std::max(0.0f, r.frameMs - r.presentMs - r.limiterMs);
    float sum = 0.0f;
    for (int c = 0; c < kCatCount; c++) sum += r.render[c];
    r.unattributedMs = std::max(0.0f, r.frameMs - sum);

    r.medianMs = MedianOfWindow();
    r.thresholdMs = std::max(g_mult.load(std::memory_order_relaxed) * r.medianMs, g_floorMs.load(std::memory_order_relaxed));
    g_lastMedian = r.medianMs;
    g_lastThreshold = r.thresholdMs;
    const bool hitch = g_medianCount >= kWarmupFrames && r.frameMs > r.thresholdMs;
    g_medianRing[g_medianPos] = r.frameMs;
    g_medianPos = (g_medianPos + 1) % kMedianWindow;
    g_medianCount = std::min(g_medianCount + 1, kMedianWindow);
    ConsumeSamples(intervalStart, now, hitch, &r.samples);
    ConsumeFrameTables(hitch, &r.detail);

    UpdateStats(r, hitch);
    if (hitch) {
        r.foreground = IsForeground();
        g_hitches[g_hitchPos] = r;
        g_hitchPos = (g_hitchPos + 1) % kHitchRing;
        g_hitchCount = std::min(g_hitchCount + 1, kHitchRing);
        if (g_writeFile.load(std::memory_order_relaxed) && g_writerRunning.load(std::memory_order_relaxed)) QueuePush(r);
    }
    RollRegistryWindow(now);
}

// ---- registry hooks ----
void AttachAllLocked();

void OnPresentStart(D3D9Hooks::DeviceContext& ctx) {
    const uint64_t now = Now();
    g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (ThreadSlot* s = GetSlot()) {
        CleanStale(s, &ctx);
        Split(s, now);
        Push(s, kPresentHooks, &ctx, kDispatch); // the bookkeeping below counts as mod Present-hook time
    }
    FrameBoundary(now);
    if (g_attachPending.load(std::memory_order_relaxed)) {
        // First frame after enabling: the timed functions are attached here, on the render thread, so the call-site
        // writes never race the render thread executing them, and after the startup patches have installed.
        // try_lock: SetEnabled holds g_ctrlMutex while it waits for the registry mutex this hook runs under.
        std::unique_lock<std::mutex> lk(g_ctrlMutex, std::try_to_lock);
        if (lk.owns_lock() && g_attachPending.load()) {
            AttachAllLocked();
            g_attachPending.store(false);
        }
    }
}

void OnDrawStart(D3D9Hooks::DeviceContext& ctx, bool indexed, UINT prims) {
    ThreadSlot* s = GetSlot();
    if (indexed) g_d3d.dip++;
    else g_d3d.dp++;
    if (s && s->open[kEndScene] > 0) {
        g_d3d.endFrameDraws++;
    } else {
        g_d3d.gameDraws++;
        g_d3d.prims += prims;
    }
    if (s) Push(s, kModD3DHooks, &ctx, kDispatch);
}

void EndDispatch(const void* sp) {
    ThreadSlot* s = t_slot;
    if (!s) return;
    for (int i = s->depth - 1; i >= 0; i--) {
        if (s->stack[i].sp == sp && (s->stack[i].flags & kDispatch)) {
            Pop(s, i, sp, Now(), 0, 0);
            return;
        }
    }
}

// Caller holds g_ctrlMutex
void RegisterD3DHooks() {
    using namespace D3D9Hooks;
    const Priority start = static_cast<Priority>(kPrioStart);
    const Priority end = static_cast<Priority>(kPrioEnd);
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnPresentStart(ctx);
        return HookAction::Continue;
    }, start);
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        EndDispatch(&ctx);
        return HookAction::Continue;
    }, end);
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT primCount) {
        OnDrawStart(ctx, true, primCount);
        return HookAction::Continue;
    }, start);
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
        EndDispatch(&ctx);
        return HookAction::Continue;
    }, end);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, UINT, UINT primCount) {
        OnDrawStart(ctx, false, primCount);
        return HookAction::Continue;
    }, start);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, UINT, UINT) {
        EndDispatch(&ctx);
        return HookAction::Continue;
    }, end);
    RegisterCreateTexture(kHookName, [](DeviceContext&, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*) {
        g_d3d.createTex++;
        return HookAction::Continue;
    }, start);
    RegisterCreateRenderTarget(kHookName, [](DeviceContext&, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) {
        g_d3d.createRT++;
        return HookAction::Continue;
    }, start);
    RegisterCreateVertexShader(kHookName, [](DeviceContext&, const DWORD*, IDirect3DVertexShader9**) {
        g_d3d.createVS++;
        return HookAction::Continue;
    }, start);
    RegisterCreatePixelShader(kHookName, [](DeviceContext&, const DWORD*, IDirect3DPixelShader9**) {
        g_d3d.createPS++;
        return HookAction::Continue;
    }, start);
    g_stateHooksActive = g_countState.load();
    if (g_stateHooksActive) { // counts only: timing every state call would cost more than it tells
        RegisterSetTexture(kHookName, [](DeviceContext&, DWORD, IDirect3DBaseTexture9*) {
            g_d3d.setTexture++;
            return HookAction::Continue;
        }, start);
        RegisterSetVertexShader(kHookName, [](DeviceContext&, IDirect3DVertexShader9*) {
            g_d3d.setShader++;
            return HookAction::Continue;
        }, start);
        RegisterSetPixelShader(kHookName, [](DeviceContext&, IDirect3DPixelShader9*) {
            g_d3d.setShader++;
            return HookAction::Continue;
        }, start);
        RegisterSetVertexShaderConstantF(kHookName, [](DeviceContext&, UINT, const float*, UINT) {
            g_d3d.shaderConst++;
            return HookAction::Continue;
        }, start);
        RegisterSetPixelShaderConstantF(kHookName, [](DeviceContext&, UINT, const float*, UINT) {
            g_d3d.shaderConst++;
            return HookAction::Continue;
        }, start);
        RegisterSetRenderTarget(kHookName, [](DeviceContext&, DWORD, IDirect3DSurface9*) {
            g_d3d.setRT++;
            return HookAction::Continue;
        }, start);
    }
}

// ---- text output ----
std::string NowString() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

const char* CameraText(int8_t c) {
    return c > 0 ? "moving" : (c == 0 ? "still" : "n/a");
}

std::string CatList(const float* v, float minMs) {
    int idx[kCatCount];
    int n = 0;
    for (int c = 0; c < kCatCount; c++)
        if (v[c] >= minMs) idx[n++] = c;
    std::sort(idx, idx + n, [&](int a, int b) { return v[a] > v[b]; });
    std::string s;
    for (int k = 0; k < n; k++) s += std::format("{}{} {:.2f}", k ? ", " : "", kCats[idx[k]].name, v[idx[k]]);
    return s;
}

double Pct(uint32_t part, uint32_t whole) {
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

// Sampling lines of one hitch (writer thread and report: reads only the append-only module table)
std::string FormatHitchSamples(const HitchSamples& m) {
    std::string s;
    static const char* const kThreadNames[2] = {"render", "simulation"};
    for (int thr = 0; thr < 2; thr++) {
        const uint32_t n = m.n[thr];
        if (!n) continue;
        s += std::format("   samples {} thread ({}):", kThreadNames[thr], n);
        for (int c = 0; c < kClsCount; c++)
            if (m.cls[thr][c]) s += std::format(" {} {:.0f}%", kClassNames[c], Pct(m.cls[thr][c], n));
        s += " | hot:";
        for (int i = 0; i < kTopN && m.fnCount[thr][i]; i++) s += std::format("{} {} {:.0f}%", i ? "," : "", KeyText(m.fnKey[thr][i]), Pct(m.fnCount[thr][i], n));
        if (thr == 0 && m.siteCount[0]) {
            s += " | on the stack:";
            for (int i = 0; i < kTopN && m.siteCount[i]; i++) s += std::format("{} {:08X} {:.0f}%", i ? "," : "", m.site[i], Pct(m.siteCount[i], n));
        }
        if (thr == 0 && m.waitCount[0]) {
            s += " | system code called from:";
            for (int i = 0; i < kTopWait && m.waitCount[i]; i++) s += std::format("{} {:08X} {:.0f}%", i ? "," : "", m.wait[i], Pct(m.waitCount[i], n));
        }
        s += "\n";
    }
    return s;
}

std::string WaitName(uint32_t key, uint32_t kind) {
    if (kind == kWaitJob) return "job wait for " + JobName(key);
    return std::format("{} at {:08X}", WaitKindName(kind), key);
}

// Services, jobs, waits and file reads of one hitch (render thread)
std::string FormatHitchDetail(const HitchDetail& d) {
    std::string s;
    if (d.svcCalls[0]) {
        s += "   services (render thread, ms incl / self):";
        for (int i = 0; i < kTopSvc && d.svcCalls[i]; i++)
            s += std::format("{} {} {:.2f} / {:.2f}", i ? "," : "", ServiceName(d.svcKey[i], d.svcVt[i]), d.svcMs[i], d.svcSelfMs[i]);
        s += "\n";
    }
    if (d.jobCalls[0]) {
        s += "   jobs run on the render thread (ms incl / self, count):";
        for (int i = 0; i < kTopJobs && d.jobCalls[i]; i++) s += std::format("{} {} {:.2f} / {:.2f} x{}", i ? "," : "", JobName(d.jobKey[i]), d.jobMs[i], d.jobSelfMs[i], d.jobCalls[i]);
        s += "\n";
    }
    if (d.waitCount[0]) {
        s += "   render-thread waits (ms, count):";
        for (int i = 0; i < kTopWaits && d.waitCount[i]; i++) s += std::format("{} {} {:.2f} x{}", i ? "," : "", WaitName(d.waitKey[i], d.waitKind[i]), d.waitMs[i], d.waitCount[i]);
        s += "\n";
    }
    if (d.readBytes) s += std::format("   file reads on the render thread: {:.1f} KB\n", d.readBytes / 1024.0);
    return s;
}

// Session tables of services / jobs / waits (render thread): ms per hitch vs ms per other frame
std::string KeyedReport() {
    const double hitches = static_cast<double>(g_stats.hitches);
    const double others = static_cast<double>(g_stats.frames - g_stats.hitches);
    const double k = g_msPerTick;
    auto perHitch = [&](uint64_t t) { return hitches > 0 ? static_cast<double>(t) * k / hitches : 0.0; };
    auto perOther = [&](uint64_t t) { return others > 0 ? static_cast<double>(t) * k / others : 0.0; };
    std::string s;
    constexpr int kRows = 40;
    int idx[kRows];
    int n = g_aSvc[1].Top(idx, kRows);
    if (n) {
        s += "Services on the render thread: ms per hitch (incl / self) | ms per other frame (incl / self) | calls in hitches\n";
        for (int i = 0; i < n; i++) {
            const auto& t = g_aSvc[1];
            const int j = idx[i];
            const int o = g_aSvc[0].Find(t.keys[j]);
            s += std::format("   {:<48} {:>8.2f} / {:>7.2f} | {:>7.2f} / {:>7.2f} | {:>8}\n", ServiceName(t.keys[j], t.aux[j]), perHitch(t.incl[j]), perHitch(t.self[j]),
                o >= 0 ? perOther(g_aSvc[0].incl[o]) : 0.0, o >= 0 ? perOther(g_aSvc[0].self[o]) : 0.0, t.calls[j]);
        }
    }
    n = g_aJob[1].Top(idx, kRows);
    if (n) {
        s += "Jobs run on the render thread: ms per hitch (incl / self) | ms per other frame (incl) | runs in hitches\n";
        for (int i = 0; i < n; i++) {
            const auto& t = g_aJob[1];
            const int j = idx[i];
            const int o = g_aJob[0].Find(t.keys[j]);
            s += std::format("   {:<48} {:>8.2f} / {:>7.2f} | {:>7.2f} | {:>8}\n", JobName(t.keys[j]), perHitch(t.incl[j]), perHitch(t.self[j]), o >= 0 ? perOther(g_aJob[0].incl[o]) : 0.0,
                t.calls[j]);
        }
    }
    n = g_aWait[1].Top(idx, kRows);
    if (n) {
        s += "Render-thread waits (job waits by job, blocked mutex / semaphore calls by return address): ms per hitch | ms per other frame | count in hitches\n";
        for (int i = 0; i < n; i++) {
            const auto& t = g_aWait[1];
            const int j = idx[i];
            const int o = g_aWait[0].Find(t.keys[j]);
            s += std::format("   {:<48} {:>8.2f} | {:>7.2f} | {:>8}\n", WaitName(t.keys[j], t.aux[j]), perHitch(t.incl[j]), o >= 0 ? perOther(g_aWait[0].incl[o]) : 0.0, t.calls[j]);
        }
    }
    if (g_aReadBytes[0] || g_aReadBytes[1])
        s += std::format("File reads on the render thread: {:.1f} KB per hitch, {:.2f} KB per other frame\n", hitches > 0 ? g_aReadBytes[1] / 1024.0 / hitches : 0.0,
            others > 0 ? g_aReadBytes[0] / 1024.0 / others : 0.0);
    bool header = false;
    for (int i = 0; i < kOtherSvcSlots; i++) {
        const uint32_t key = g_otherSvc[i].key.load();
        if (!key) continue;
        if (!header) {
            s += "Services on other threads (simulation loop), since Clear: total ms incl / self | calls\n";
            header = true;
        }
        const uint64_t in = g_otherSvc[i].incl.load() - g_otherSvcBaseIncl[i], se = g_otherSvc[i].self.load() - g_otherSvcBaseSelf[i];
        s += std::format("   {:<48} {:>10.1f} / {:>10.1f} | {:>8}\n", ServiceName(key, g_otherSvc[i].vtable.load()), static_cast<double>(in) * k, static_cast<double>(se) * k,
            g_otherSvc[i].calls.load() - g_otherSvcBaseCalls[i]);
    }
    return s;
}

std::string SamplerStatusText() {
    const uint32_t taken = g_sTaken.load();
    const double costUs = taken ? static_cast<double>(g_sPausedTicks.load()) * g_msPerTick * 1000.0 / taken : 0.0;
    return std::format("Sampler {} ({} Hz target): {} samples, {:.1f} us paused per sample, {} dropped (ring full)", g_samplerRunning.load() ? "on" : "off",
        g_sampleHz.load(), taken, costUs, g_sDropped.load());
}

// Render thread: session tables (hitch frames vs other frames), sorted by samples in hitch frames
std::string SamplingReport() {
    const SampleAgg& hi = g_agg[1];
    const SampleAgg& lo = g_agg[0];
    if (!hi.total[0] && !hi.total[1] && !lo.total[0] && !lo.total[1]) return "";
    std::string s = "Sampling (return addresses from a heuristic stack walk; 'fn~' = TS3W function start guessed from the int3 padding,\n"
                    "resolve it with the decompile; % = share of that thread's samples in hitch frames / in other frames):\n";
    s += "   " + SamplerStatusText() + "\n";
    static const char* const kThreadNames[2] = {"Render", "Simulation"};
    constexpr int kRows = 40;
    uint32_t keys[kRows], counts[kRows];
    for (int thr = 0; thr < 2; thr++) {
        if (!hi.total[thr] && !lo.total[thr]) continue;
        s += std::format("{} thread: {} samples in hitch frames, {} in other frames\n", kThreadNames[thr], hi.total[thr], lo.total[thr]);
        s += "   code class          hitch %   other %\n";
        for (int c = 0; c < kClsCount; c++)
            s += std::format("   {:<18} {:>8.1f} {:>9.1f}\n", kClassNames[c], Pct(hi.cls[thr][c], hi.total[thr]), Pct(lo.cls[thr][c], lo.total[thr]));
        s += "   hottest code (hitch frames)      samples  hitch %  other %  class\n";
        int n = hi.fn[thr].Top(keys, counts, kRows);
        for (int i = 0; i < n; i++)
            s += std::format("   {:<32} {:>8} {:>8.1f} {:>8.1f}  {}\n", KeyText(keys[i]), counts[i], Pct(counts[i], hi.total[thr]), Pct(lo.fn[thr].Get(keys[i]), lo.total[thr]),
                kClassNames[KeyClass(keys[i])]);
        if (thr != 0) continue;
        s += "   TS3W call sites on the stack (return address)  samples  hitch %  other %\n";
        n = hi.site.Top(keys, counts, kRows);
        for (int i = 0; i < n; i++)
            s += std::format("   {:08X}                                  {:>8} {:>8.1f} {:>8.1f}\n", keys[i], counts[i], Pct(counts[i], hi.total[0]), Pct(lo.site.Get(keys[i]), lo.total[0]));
        s += "   system code (ntdll/kernel/user32...) called from  samples  hitch %  other %\n";
        n = hi.wait.Top(keys, counts, 20);
        for (int i = 0; i < n; i++)
            s += std::format("   {:08X}                                  {:>8} {:>8.1f} {:>8.1f}\n", keys[i], counts[i], Pct(counts[i], hi.total[0]), Pct(lo.wait.Get(keys[i]), lo.total[0]));
        s += "   exact EIPs in hitch frames                  samples  hitch %  module\n";
        n = g_hotEip.Top(keys, counts, kRows);
        for (int i = 0; i < n; i++) {
            const int mi = FindModule(keys[i]);
            s += std::format("   {:08X}                                  {:>8} {:>8.1f}  {}\n", keys[i], counts[i], Pct(counts[i], hi.total[0]), mi >= 0 ? g_modules[mi].name : "?");
        }
    }
    return s;
}

std::string FormatHitch(const HitchRecord& h) {
    std::string s = std::format("#{} t={:.1f}s  frame {:.2f} ms (median {:.2f}, threshold {:.2f})  cpu {:.2f}  present {:.2f}  limiter {:.2f}  mod D3D {:.2f}  camera {}{}\n", h.frame, h.tSec,
        h.frameMs, h.medianMs, h.thresholdMs, h.cpuMs, h.presentMs, h.limiterMs, h.modMs, CameraText(h.camera), h.foreground ? "" : "  (window in background)");
    const std::string render = CatList(h.render, 0.05f);
    s += std::format("   render thread (self ms): {}{}Unattributed {:.2f}\n", render, render.empty() ? "" : ", ", h.unattributedMs);
    float nested[kCatCount];
    for (int c = 0; c < kCatCount; c++) nested[c] = h.renderIncl[c] > h.render[c] + 0.05f ? h.renderIncl[c] : 0.0f;
    if (const std::string incl = CatList(nested, 0.05f); !incl.empty()) s += "   render thread (incl. nested ms): " + incl + "\n";
    if (const std::string sim = CatList(h.sim, 0.05f); !sim.empty()) s += "   simulation thread (self ms): " + sim + "\n";
    if (const std::string other = CatList(h.other, 0.05f); !other.empty()) s += "   other threads (self ms): " + other + "\n";
    std::string calls;
    for (int c = kFirstGameCat; c < kCatCount; c++)
        if (h.calls[c]) calls += std::format("{}{} x{}", calls.empty() ? "" : ", ", kCats[c].name, h.calls[c]);
    if (!calls.empty()) s += "   calls: " + calls + "\n";
    s += std::format("   d3d: draws {} (+{} end-of-frame), prims {}, created: textures {}, render targets {}, vertex shaders {}, pixel shaders {}", h.gameDraws, h.endFrameDraws, h.prims,
        h.createTex, h.createRT, h.createVS, h.createPS);
    if (h.stateCounted) s += std::format("; SetTexture {}, Set*Shader {}, shader constants {}, SetRenderTarget {}", h.setTexture, h.setShader, h.shaderConst, h.setRT);
    s += std::format("\n   lots promoted {}, demoted {}\n", h.lotsPromoted, h.lotsDemoted);
    s += FormatHitchDetail(h.detail);
    s += FormatHitchSamples(h.samples);
    return s;
}

std::string ClockText() {
    if (g_msPerTick <= 0.0) return "not started";
    return g_useTsc ? std::format("RDTSC (invariant, {:.3f} GHz)", 1.0 / (g_msPerTick * 1e6)) : std::string("QueryPerformanceCounter");
}

// Caller holds g_ctrlMutex
std::string HookStatusText() {
    std::string s;
    for (int i = 0; i < kTargetCount; i++)
        s += std::format("   {:<44} {:#010x}  expected thread: {:<30} {}\n", kTargets[i].name, DisplayAddress(i), kTargets[i].thread, g_targets[i].status);
    return s;
}

std::string SessionHeader() {
    return std::format("\n==== Frame profiler session {} | game {} | clock {} | hitch = frame > max({:.1f} x median of the last {} frames, {:.1f} ms) ====\n"
                       "Times are ms per frame interval (Present to Present); 'self' = exclusive of nested timed calls; see frame_profiler.cpp.\n",
        NowString(), GetGameVersionName(), ClockText(), g_mult.load(), kMedianWindow, g_floorMs.load());
}

void AppendFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::out | std::ios::app | std::ios::binary);
    if (f) f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// ---- writer thread ----
// A raw Win32 thread, not a global std::thread: a joinable std::thread destroyed at process exit calls std::terminate.
HANDLE g_writerThread = nullptr;
std::atomic<bool> g_writerStop{false};
HANDLE g_wake = nullptr;
std::mutex g_pendingMutex;
std::string g_pendingText;
std::filesystem::path g_filePath;

struct WriterArgs {
    std::filesystem::path path;
    std::string header;
};

void WriterMain(const std::filesystem::path& path, const std::string& header) {
    AppendFile(path, header);
    std::vector<HitchRecord> batch;
    batch.reserve(kQueueSize);
    for (;;) {
        const bool stop = g_writerStop.load();
        batch.clear();
        uint32_t t = g_qTail.load(std::memory_order_relaxed);
        const uint32_t h = g_qHead.load(std::memory_order_acquire);
        while (t != h) {
            batch.push_back(g_queue[t % kQueueSize]);
            t++;
        }
        g_qTail.store(t, std::memory_order_release);
        std::string out;
        for (const auto& r : batch) out += FormatHitch(r);
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            out += g_pendingText;
            g_pendingText.clear();
        }
        if (!out.empty()) AppendFile(path, out);
        g_qWritten.fetch_add(static_cast<uint32_t>(batch.size()), std::memory_order_relaxed);
        if (stop) break;
        WaitForSingleObject(g_wake, 1000); // batches at most once per second (sooner only for "Save report now")
    }
}

DWORD WINAPI WriterThreadProc(LPVOID param) {
    const std::unique_ptr<WriterArgs> args(static_cast<WriterArgs*>(param));
    WriterMain(args->path, args->header);
    return 0;
}

// Caller holds g_ctrlMutex
void StartWriter() {
    if (g_writerThread) return;
    ApexPaths::EnsureApexDirectory();
    g_filePath = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexRadiance_Hitches.txt";
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) {
        LOG_ERROR("[FrameProfiler] CreateEvent failed, ApexRadiance_Hitches.txt will not be written");
        return;
    }
    g_writerStop.store(false);
    g_qTail.store(g_qHead.load()); // nothing queued is carried over into a new session
    auto* args = new WriterArgs{g_filePath, SessionHeader()};
    g_writerThread = CreateThread(nullptr, 0, WriterThreadProc, args, 0, nullptr);
    if (!g_writerThread) {
        delete args;
        LOG_ERROR("[FrameProfiler] CreateThread failed, ApexRadiance_Hitches.txt will not be written");
        return;
    }
    g_writerRunning.store(true);
}

// Caller holds g_ctrlMutex. Runs on the render thread at the first frame boundary after enabling.
void AttachAllLocked() {
    int timed = 0, total = 0;
    for (int i = 0; i < kTargetCount; i++) {
        if (kTargets[i].optional && !g_objectBuildWanted) {
            g_targets[i].status = "Off (optional: Advanced > Time lot object building)";
            continue;
        }
        total++;
        if (AttachTarget(i)) timed++;
        else LOG_WARNING(std::format("[FrameProfiler] {}: {}", kTargets[i].name, g_targets[i].status));
    }
    g_summary = std::format("Timing {} of {} game functions", timed, total);
    LOG_INFO("[FrameProfiler] " + g_summary);
    if (g_writerRunning.load()) {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingText += "Timed functions:\n" + HookStatusText();
    }
}

// Caller holds g_ctrlMutex
void StopWriter() {
    g_writerRunning.store(false);
    if (!g_writerThread) return;
    g_writerStop.store(true);
    SetEvent(g_wake);
    WaitForSingleObject(g_writerThread, 5000);
    CloseHandle(g_writerThread);
    g_writerThread = nullptr;
}

// ---- report ----
LiveSample AverageLive() {
    LiveSample a{};
    if (!g_liveCount) return a;
    for (int i = 0; i < g_liveCount; i++) {
        const LiveSample& l = g_live[i];
        a.frameMs += l.frameMs;
        a.cpuMs += l.cpuMs;
        a.presentMs += l.presentMs;
        a.limiterMs += l.limiterMs;
        a.modMs += l.modMs;
        a.gameDraws += l.gameDraws;
        a.endFrameDraws += l.endFrameDraws;
        a.setTexture += l.setTexture;
        a.setShader += l.setShader;
        a.shaderConst += l.shaderConst;
        a.setRT += l.setRT;
    }
    const float n = static_cast<float>(g_liveCount);
    a.frameMs /= n;
    a.cpuMs /= n;
    a.presentMs /= n;
    a.limiterMs /= n;
    a.modMs /= n;
    a.gameDraws /= n;
    a.endFrameDraws /= n;
    a.setTexture /= n;
    a.setShader /= n;
    a.shaderConst /= n;
    a.setRT /= n;
    return a;
}

struct HitchAggregate {
    int count = 0, moving = 0, still = 0;
    double frameMs = 0, medianMs = 0, unattributed = 0;
    double render[kCatCount] = {}, others[kCatCount] = {}, worst[kCatCount] = {};
    uint64_t calls[kCatCount] = {};
    uint64_t createTex = 0, createShaders = 0, promoted = 0;
};

HitchAggregate AggregateHitches() {
    HitchAggregate a;
    for (int k = 0; k < g_hitchCount; k++) {
        const HitchRecord& h = g_hitches[k];
        a.count++;
        a.frameMs += h.frameMs;
        a.medianMs += h.medianMs;
        a.unattributed += h.unattributedMs;
        if (h.camera == 1) a.moving++;
        if (h.camera == 0) a.still++;
        for (int c = 0; c < kCatCount; c++) {
            a.render[c] += h.render[c];
            const double o = static_cast<double>(h.sim[c]) + h.other[c];
            a.others[c] += o;
            a.worst[c] = std::max(a.worst[c], static_cast<double>(h.render[c]) + o);
            a.calls[c] += h.calls[c];
        }
        a.createTex += h.createTex;
        a.createShaders += h.createVS + h.createPS;
        a.promoted += h.lotsPromoted;
    }
    return a;
}

std::string GcCallSiteText() {
    static uintptr_t site = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (g_gameVersion == GameVersion::Steam) {
            if (MatchAt(0x00D819A0, "68 ?? ?? ?? ?? A3")) site = 0x00D819AA;
        } else if (const uintptr_t a = ScanUnique("68 ?? ?? ?? ?? A3 ?? ?? ?? ?? ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04 3B 05")) {
            site = a + 10;
        }
    }
    if (!site) return "call site in MonoScriptHost::Simulate not found";
    const uint8_t op = *reinterpret_cast<const uint8_t*>(site);
    if (op == 0x90) return "call removed (Chunky Patch - Disable GC_try_to_collect): no explicit collections to measure";
    if (op == 0xE8) {
        int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
        const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        if (target == g_targets[T_ScriptGC].addr || target == kTargets[T_ScriptGC].steam) return "calls GC_try_to_collect directly";
        return std::format("redirected to {:#010x} by another patch (e.g. Script GC Scheduler); timed whenever it calls GC_try_to_collect", target);
    }
    return "modified by another patch";
}

std::string LimiterText() {
    const uintptr_t fn = g_targets[T_RenderFrame].addr;
    if (!fn) return "unknown (render frame function not resolved)";
    const uintptr_t site = fn + 0xBA; // Steam 1.67.2: 0x00EC9FBA, the site Smooth Patch replaces
    if (MatchAt(site, "80 BE 8D 00 00 00 00 5E 74 15")) return "game default (sleeps ~30 ms per frame only while the window is inactive)";
    if (MatchAt(site, "3E 56 E8 ?? ?? ?? ?? 5E EB")) return "Smooth Patch frame limiter";
    return "modified by another patch";
}

std::string BuildReport() {
    const LiveSample a = AverageLive();
    std::string s = std::format("\n==== Frame profiler report {} ====\n", NowString());
    s += std::format("Game {} | clock {} | hitch = frame > max({:.1f} x median of the last {} frames, {:.1f} ms)\n", GetGameVersionName(), ClockText(), g_mult.load(), kMedianWindow,
        g_floorMs.load());
    s += std::format("Frames {} | hitches {} ({:.2f}%) | p50 {:.2f} ms | p95 {:.2f} | p99 {:.2f} | max {:.1f}\n", g_stats.frames, g_stats.hitches,
        g_stats.frames ? 100.0 * static_cast<double>(g_stats.hitches) / static_cast<double>(g_stats.frames) : 0.0, Percentile(0.5), Percentile(0.95), Percentile(0.99), g_stats.maxFrameMs);
    s += std::format("Last {} frames: frame {:.2f} ms, cpu {:.2f}, present {:.2f}, limiter {:.2f}, mod D3D {:.2f}, draws {:.0f} (+{:.0f} end-of-frame)\n", g_liveCount, a.frameMs, a.cpuMs,
        a.presentMs, a.limiterMs, a.modMs, a.gameDraws, a.endFrameDraws);
    const auto rate = [](uint64_t h, uint64_t f) { return f ? 100.0 * static_cast<double>(h) / static_cast<double>(f) : 0.0; };
    s += std::format("Camera: moving in {} frames, still in {}; hitch rate while moving {:.2f}%, while still {:.2f}%\n", g_stats.framesMoving, g_stats.framesStill,
        rate(g_stats.hitchesMoving, g_stats.framesMoving), rate(g_stats.hitchesStill, g_stats.framesStill));
    {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        s += "Hooks:\n" + HookStatusText();
    }
    s += "GC call site: " + GcCallSiteText() + "\nFrame limiter: " + LimiterText() + "\n";
    s += "Totals since Clear (self ms): category | render thread | simulation thread | other threads | calls\n";
    for (int c = 0; c < kCatCount; c++) {
        const uint64_t calls = g_stats.bucketCalls[0][c] + g_stats.bucketCalls[1][c] + g_stats.bucketCalls[2][c];
        if (calls || g_stats.render[c] > 0.0 || g_stats.sim[c] > 0.0 || g_stats.other[c] > 0.0)
            s += std::format("   {:<24} {:>12.1f} {:>12.1f} {:>12.1f} {:>10}\n", kCats[c].name, g_stats.render[c], g_stats.sim[c], g_stats.other[c], calls);
    }
    s += std::format("   {:<24} {:>12.1f}\n", "Unattributed (render)", g_stats.unattributed);
    const HitchAggregate h = AggregateHitches();
    if (h.count) {
        s += std::format("Last {} hitches: average frame {:.2f} ms (median before them {:.2f}), camera moving in {}, still in {}; per hitch (self ms): category | render | other threads | worst\n",
            h.count, h.frameMs / h.count, h.medianMs / h.count, h.moving, h.still);
        for (int c = 0; c < kCatCount; c++)
            if (h.render[c] + h.others[c] > 0.0)
                s += std::format("   {:<24} {:>8.2f} {:>8.2f} {:>8.2f}\n", kCats[c].name, h.render[c] / h.count, h.others[c] / h.count, h.worst[c]);
        s += std::format("   {:<24} {:>8.2f}\n", "Unattributed (render)", h.unattributed / h.count);
        s += "Hitches (oldest first):\n";
        const int first = (g_hitchPos - g_hitchCount + kHitchRing) % kHitchRing;
        for (int k = 0; k < g_hitchCount; k++) s += FormatHitch(g_hitches[(first + k) % kHitchRing]);
    }
    s += KeyedReport();
    s += SamplingReport();
    s += "==== end of report ====\n";
    return s;
}

void SaveReport() {
    std::string text = BuildReport();
    if (g_writerRunning.load()) {
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            g_pendingText += text;
        }
        if (g_wake) SetEvent(g_wake);
        return;
    }
    // profiler off: a one-shot thread, so the file I/O stays off the render thread
    ApexPaths::EnsureApexDirectory();
    std::filesystem::path path = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexRadiance_Hitches.txt";
    std::thread([path, t = std::move(text)] { AppendFile(path, t); }).detach();
}

void Clear() {
    ClearSampleAggregates();
    for (int b = 0; b < 2; b++) {
        g_aSvc[b].Clear();
        g_aJob[b].Clear();
        g_aWait[b].Clear();
        g_aReadBytes[b] = 0;
    }
    for (int i = 0; i < kOtherSvcSlots; i++) {
        g_otherSvcBaseIncl[i] = g_otherSvc[i].incl.load();
        g_otherSvcBaseSelf[i] = g_otherSvc[i].self.load();
        g_otherSvcBaseCalls[i] = g_otherSvc[i].calls.load();
    }
    g_stats = Stats{};
    g_graphPos = g_graphCount = 0;
    g_livePos = g_liveCount = 0;
    g_medianPos = g_medianCount = 0;
    g_hitchPos = g_hitchCount = 0;
    g_lastThreshold = g_lastMedian = 0.0f;
    g_regDisplay.clear();
}

// ---- UI ----
void Hint(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void RenderLive() {
    const LiveSample a = AverageLive();
    ImGui::Text("Frame %.2f ms (%.0f FPS)   CPU %.2f   Present %.2f   Limiter %.2f", a.frameMs, a.frameMs > 0.0f ? 1000.0f / a.frameMs : 0.0f, a.cpuMs, a.presentMs, a.limiterMs);
    Hint("Averages of the last 60 frames.\nCPU = frame time minus the time blocked in Present and in the frame limiter.\n"
         "High CPU = game or draw submission bound. High Present = GPU or vsync bound.");
    ImGui::Text("Draws %.0f / frame (+%.0f end-of-frame)   Mod D3D hooks %.2f ms", a.gameDraws, a.endFrameDraws, a.modMs);
    Hint("Game draw calls (DrawIndexedPrimitive + DrawPrimitive) per frame; end-of-frame = draws inside EndScene (menus, Picture pass).\n"
         "DrawPrimitiveUP / DrawIndexedPrimitiveUP are not counted.\n"
         "Mod D3D hooks = time all Apex modules spend in their D3D9 hooks (draws and Present) per frame.");
    if (g_stateHooksActive) {
        ImGui::TextDisabled("Per frame: SetTexture %.0f   Set*Shader %.0f   Shader constants %.0f   SetRenderTarget %.0f", a.setTexture, a.setShader, a.shaderConst, a.setRT);
    }

    float values[kGraphFrames];
    const int n = g_graphCount;
    for (int k = 0; k < n; k++) values[k] = g_graph[(g_graphPos - n + k + kGraphFrames) % kGraphFrames];
    char overlay[96];
    std::snprintf(overlay, sizeof overlay, "last %.1f ms   hitch above %.1f ms", n ? values[n - 1] : 0.0f, g_lastThreshold);
    const float top = std::min(250.0f, std::max(33.4f, g_lastThreshold * 1.5f));
    ImGui::PlotLines("##FrameTimes", values, n, 0, overlay, 0.0f, top, ImVec2(ImGui::GetContentRegionAvail().x, 80.0f));
    Hint("Frame time of the last 300 frames (ms). The scale tops out at 1.5 x the hitch threshold.");

    ImGui::Text("p50 %.2f   p95 %.2f   p99 %.2f   max %.1f ms", Percentile(0.5), Percentile(0.95), Percentile(0.99), g_stats.maxFrameMs);
    Hint("Frame-time percentiles since Clear (0.05 ms resolution below 100 ms).");
    ImGui::Text("%llu frames, %llu hitches (%.2f%% over the threshold)", static_cast<unsigned long long>(g_stats.frames), static_cast<unsigned long long>(g_stats.hitches),
        g_stats.frames ? 100.0 * static_cast<double>(g_stats.hitches) / static_cast<double>(g_stats.frames) : 0.0);
    Hint("A hitch is a frame longer than max(multiplier x median of the last 120 frames, floor). Both are in Advanced.");
    const auto rate = [](uint64_t h, uint64_t f) { return f ? 100.0 * static_cast<double>(h) / static_cast<double>(f) : 0.0; };
    if (g_stats.framesMoving + g_stats.framesStill) {
        ImGui::TextDisabled("Camera moving in %.0f%% of frames. Hitch rate: %.2f%% moving, %.2f%% still", rate(g_stats.framesMoving, g_stats.framesMoving + g_stats.framesStill),
            rate(g_stats.hitchesMoving, g_stats.framesMoving), rate(g_stats.hitchesStill, g_stats.framesStill));
    } else {
        ImGui::TextDisabled("Camera motion: not seen yet (needs a loaded world)");
    }
}

void RenderHitches() {
    const HitchAggregate h = AggregateHitches();
    if (ImGui::TreeNodeEx("Last hitches", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!h.count) {
            ImGui::TextDisabled("No hitch yet.");
        } else {
            ImGui::TextDisabled("%d hitches: average %.1f ms (median before them %.1f ms); camera moving in %d, still in %d", h.count, h.frameMs / h.count, h.medianMs / h.count, h.moving,
                h.still);
            int order[kCatCount];
            int n = 0;
            for (int c = 0; c < kCatCount; c++)
                if (h.render[c] + h.others[c] > 0.005 * h.count) order[n++] = c;
            std::sort(order, order + n, [&](int a, int b) { return h.render[a] + h.others[a] > h.render[b] + h.others[b]; });
            if (ImGui::BeginTable("##HitchCats", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Category");
                ImGui::TableSetupColumn("Render ms");
                ImGui::TableSetupColumn("Other threads ms");
                ImGui::TableSetupColumn("Worst ms");
                ImGui::TableSetupColumn("Calls");
                ImGui::TableHeadersRow();
                for (int k = 0; k < n; k++) {
                    const int c = order[k];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kCats[c].name);
                    Hint(kCats[c].hint);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", h.render[c] / h.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", h.others[c] / h.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", h.worst[c]);
                    ImGui::TableNextColumn();
                    if (c >= kFirstGameCat) ImGui::Text("%.1f", static_cast<double>(h.calls[c]) / h.count);
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Unattributed (render)");
                Hint("Render-thread time in code that is not timed: game update, UI, scripts on the main thread, everything else.");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", h.unattributed / h.count);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Per hitch, self time. Created per hitch: %.1f textures, %.1f shaders; %.2f lots promoted", static_cast<double>(h.createTex) / h.count,
                static_cast<double>(h.createShaders) / h.count, static_cast<double>(h.promoted) / h.count);
        }
        ImGui::TreePop();
    }
    if (h.count && ImGui::TreeNode("Recent hitches")) {
        const int shown = std::min(g_hitchCount, 25);
        for (int k = 0; k < shown; k++) {
            const HitchRecord& r = g_hitches[(g_hitchPos - 1 - k + kHitchRing) % kHitchRing];
            float all[kCatCount];
            for (int c = 0; c < kCatCount; c++) all[c] = r.render[c] + r.sim[c] + r.other[c];
            int top[3] = {-1, -1, -1};
            for (int c = 0; c < kCatCount; c++) {
                if (all[c] < 0.05f) continue;
                for (int j = 0; j < 3; j++) {
                    if (top[j] < 0 || all[c] > all[top[j]]) {
                        for (int m = 2; m > j; m--) top[m] = top[m - 1];
                        top[j] = c;
                        break;
                    }
                }
            }
            std::string line = std::format("{:.1f} ms  camera {}  draws {}  |", r.frameMs, CameraText(r.camera), r.gameDraws);
            for (int j = 0; j < 3; j++)
                if (top[j] >= 0) line += std::format("  {} {:.1f}", kCats[top[j]].name, all[top[j]]);
            line += std::format("  Unattributed {:.1f}", r.unattributedMs);
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::TreePop();
    }
}

// Services / jobs / waits of the render thread, hitch frames vs other frames
template <uint32_t N, typename NameFn>
void KeyedTable(const char* id, const TimeTable<N>& hi, const TimeTable<N>& lo, NameFn name, bool showSelf) {
    const double hitches = static_cast<double>(g_stats.hitches), others = static_cast<double>(g_stats.frames - g_stats.hitches);
    constexpr int kRows = 15;
    int idx[kRows];
    const int n = hi.Top(idx, kRows);
    if (!n) {
        ImGui::TextDisabled("Nothing in hitch frames yet.");
        return;
    }
    if (!ImGui::BeginTable(id, showSelf ? 5 : 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("ms / hitch");
    if (showSelf) ImGui::TableSetupColumn("self ms / hitch");
    ImGui::TableSetupColumn("ms / other frame");
    ImGui::TableSetupColumn("Count in hitches");
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; i++) {
        const int j = idx[i];
        const int o = lo.Find(hi.keys[j]);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name(hi.keys[j], hi.aux[j]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", hitches > 0 ? static_cast<double>(hi.incl[j]) * g_msPerTick / hitches : 0.0);
        if (showSelf) {
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", hitches > 0 ? static_cast<double>(hi.self[j]) * g_msPerTick / hitches : 0.0);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", o >= 0 && others > 0 ? static_cast<double>(lo.incl[o]) * g_msPerTick / others : 0.0);
        ImGui::TableNextColumn();
        ImGui::Text("%u", hi.calls[j]);
    }
    ImGui::EndTable();
}

void RenderKeyed() {
    ImGui::SeparatorText("Services, jobs and waits");
    if (ImGui::TreeNode("Services (render thread)")) {
        ImGui::TextDisabled("Each service update of the main loop, incl. = with the timed functions inside it, self = without.");
        KeyedTable("##svc", g_aSvc[1], g_aSvc[0], [](uint32_t k, uint32_t aux) { return ServiceName(k, aux); }, true);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Jobs run on the render thread")) {
        ImGui::TextDisabled("By job function; remote calls by the method they run.");
        KeyedTable("##job", g_aJob[1], g_aJob[0], [](uint32_t k, uint32_t) { return JobName(k); }, true);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Render-thread waits")) {
        ImGui::TextDisabled("Job waits by job; mutex / semaphore calls that blocked > 0.1 ms by return address.");
        KeyedTable("##wait", g_aWait[1], g_aWait[0], [](uint32_t k, uint32_t aux) { return WaitName(k, aux); }, false);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Services on other threads")) {
        ImGui::TextDisabled("The simulation loop (FUN_0059ED70) and any service update outside the render thread, total since Clear.");
        for (int i = 0; i < kOtherSvcSlots; i++) {
            const uint32_t key = g_otherSvc[i].key.load();
            if (!key) continue;
            const uint64_t in = g_otherSvc[i].incl.load() - g_otherSvcBaseIncl[i], se = g_otherSvc[i].self.load() - g_otherSvcBaseSelf[i];
            ImGui::Text("%-44s %9.1f ms (self %.1f), %u calls", ServiceName(key, g_otherSvc[i].vtable.load()).c_str(), static_cast<double>(in) * g_msPerTick,
                static_cast<double>(se) * g_msPerTick, g_otherSvc[i].calls.load() - g_otherSvcBaseCalls[i]);
        }
        ImGui::TreePop();
    }
}

// One sampling table: code (or call site) | hitch % | other frames % | module
void SamplingTable(const char* id, const char* first, uint32_t* keys, uint32_t* counts, int n, bool sites, uint32_t hiTotal, uint32_t loTotal,
    uint32_t (*loGet)(uint32_t)) {
    if (!n || !ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn(first);
    ImGui::TableSetupColumn("Hitch %");
    ImGui::TableSetupColumn("Other frames %");
    ImGui::TableSetupColumn("Class");
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (sites) ImGui::Text("%08X", keys[i]);
        else ImGui::TextUnformatted(KeyText(keys[i]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", Pct(counts[i], hiTotal));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", Pct(loGet(keys[i]), loTotal));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sites ? "TS3W" : kClassNames[KeyClass(keys[i])]);
    }
    ImGui::EndTable();
}

void RenderSampling(bool& save) {
    ImGui::SeparatorText("Sampling");
    bool render = g_sampleRender.load(), sim = g_sampleSim.load();
    bool changed = false;
    if (ImGui::Checkbox("Sample the render thread", &render)) changed = true;
    Hint("A sampler thread pauses the render thread ~2000 times a second for a few microseconds and records where it is\n"
         "(EIP and the TS3W call sites on its stack). Hitches then show which code the Unattributed time was spent in.\n"
         "Costs the paused time shown below (a few % of the render thread) plus a little CPU on another core.");
    if (ImGui::Checkbox("Sample the simulation thread", &sim)) changed = true;
    Hint("Same for the simulation thread (the thread that calls GC_try_to_collect; unknown until the first script GC).");
    int hz = g_sampleHz.load();
    if (ImGui::SliderInt("Sampling rate", &hz, 250, 4000, "%d Hz")) g_sampleHz.store(hz);
    save |= ImGui::IsItemDeactivatedAfterEdit();
    Hint("Samples per second and per thread. Higher = finer detail, more pause time.");
    if (changed) {
        g_sampleRender.store(render);
        g_sampleSim.store(sim);
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        UpdateSamplerLocked();
        save = true;
    }
    // measured rate, refreshed once a second
    static uint64_t lastTick = 0;
    static uint32_t lastTaken = 0;
    static float rate = 0.0f;
    const uint64_t tick = GetTickCount64();
    if (tick - lastTick >= 1000) {
        const uint32_t taken = g_sTaken.load();
        rate = lastTick ? static_cast<float>(taken - lastTaken) * 1000.0f / static_cast<float>(tick - lastTick) : 0.0f;
        lastTick = tick;
        lastTaken = taken;
    }
    const uint32_t taken = g_sTaken.load();
    const double costUs = taken ? static_cast<double>(g_sPausedTicks.load()) * g_msPerTick * 1000.0 / taken : 0.0;
    ImGui::TextDisabled("Sampler %s: %.0f samples/s, %.1f us paused per sample (~%.1f%% of a sampled thread), %u dropped", g_samplerRunning.load() ? "on" : "off", rate, costUs,
        rate > 0.0f && (render || sim) ? rate / ((render && sim) ? 2.0 : 1.0) * costUs / 10000.0 : 0.0, g_sDropped.load());
    const DWORD simTid = g_simTid.load();
    ImGui::TextDisabled("Render thread %lu, simulation thread %s", static_cast<unsigned long>(g_renderTid.load()), simTid ? std::to_string(simTid).c_str() : "not identified yet");

    const SampleAgg& hi = g_agg[1];
    const SampleAgg& lo = g_agg[0];
    for (int thr = 0; thr < 2; thr++) {
        if (!hi.total[thr] && !lo.total[thr]) continue;
        ImGui::PushID(thr);
        if (ImGui::TreeNode(thr == 0 ? "Render thread samples" : "Simulation thread samples")) {
            ImGui::TextDisabled("%u samples in hitch frames, %u in other frames", hi.total[thr], lo.total[thr]);
            std::string line = "Hitch frames:";
            for (int c = 0; c < kClsCount; c++) line += std::format("  {} {:.0f}%", kClassNames[c], Pct(hi.cls[thr][c], hi.total[thr]));
            ImGui::TextUnformatted(line.c_str());
            line = "Other frames:";
            for (int c = 0; c < kClsCount; c++) line += std::format("  {} {:.0f}%", kClassNames[c], Pct(lo.cls[thr][c], lo.total[thr]));
            ImGui::TextUnformatted(line.c_str());
            Hint("Where the thread's time went: TS3W = the game's own code, DXVK/driver = d3d9.dll / dxgi / Vulkan driver,\n"
                 "system = ntdll / kernel32 / kernelbase / user32 (waits, heap, I/O), Apex = this mod (official S3SS counts as other ASI).");
            constexpr int kRows = 20;
            uint32_t keys[kRows], counts[kRows];
            static const SampleAgg* s_lo = nullptr;
            static int s_thr = 0;
            s_lo = &lo;
            s_thr = thr;
            ImGui::TextDisabled("Hottest code in hitch frames ('fn~' = TS3W function start, guessed; resolve it with the decompile)");
            int n = hi.fn[thr].Top(keys, counts, kRows);
            SamplingTable("##fn", "Code", keys, counts, n, false, hi.total[thr], lo.total[thr], [](uint32_t k) { return s_lo->fn[s_thr].Get(k); });
            if (thr == 0) {
                ImGui::TextDisabled("TS3W call sites on the stack (return addresses: who called into the hot code)");
                n = hi.site.Top(keys, counts, kRows);
                SamplingTable("##site", "Return address", keys, counts, n, true, hi.total[0], lo.total[0], [](uint32_t k) { return s_lo->site.Get(k); });
                ImGui::TextDisabled("System code (waits, heap, I/O) called from");
                n = hi.wait.Top(keys, counts, 10);
                SamplingTable("##wait", "Return address", keys, counts, n, true, hi.total[0], lo.total[0], [](uint32_t k) { return s_lo->wait.Get(k); });
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void RenderAdvanced() {
    if (!ApexUi::AdvancedNode("Advanced##FrameProfiler")) return;
    bool save = false;
    float mult = g_mult.load();
    if (ImGui::SliderFloat("Hitch multiplier", &mult, 1.2f, 5.0f, "%.1fx")) g_mult.store(mult);
    save |= ImGui::IsItemDeactivatedAfterEdit();
    Hint("A frame is a hitch when it is longer than this many times the median of the last 120 frames (and than the floor).");
    float floorMs = g_floorMs.load();
    if (ImGui::SliderFloat("Hitch floor", &floorMs, 1.0f, 100.0f, "%.0f ms")) g_floorMs.store(floorMs);
    save |= ImGui::IsItemDeactivatedAfterEdit();
    Hint("Frames shorter than this are never hitches.");
    bool state = g_countState.load();
    if (ImGui::Checkbox("Count state calls", &state)) {
        g_countState.store(state);
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        if (g_enabled.load()) {
            D3D9Hooks::UnregisterAll(kHookName);
            RegisterD3DHooks();
        }
        save = true;
    }
    Hint("Also count SetTexture, Set*Shader, Set*ShaderConstantF and SetRenderTarget per frame. Costs a few ns per call.");
    bool file = g_writeFile.load();
    if (ImGui::Checkbox("Write ApexRadiance_Hitches.txt", &file)) {
        g_writeFile.store(file);
        save = true;
    }
    Hint("Append every hitch to ApexRadiance_Hitches.txt in the Apex Radiance folder (Documents), in batches at most once per second.");
    bool objects = g_objectBuildWanted;
    if (ImGui::Checkbox("Time lot object building (this session)", &objects)) {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        g_objectBuildWanted = objects;
        if (g_enabled.load() && !g_attachPending.load()) {
            if (objects) AttachTarget(T_LotObjectBuild);
            else DetachTarget(T_LotObjectBuild);
        }
    }
    Hint("Also time Lot::UpdateObjectSceneNode (every object's scene node when a lot gets detailed).\n"
         "While on, Lot Streaming Optimizations cannot re-install (changing its settings turns its object throttle off),\n"
         "so leave its settings alone during the measurement. Not saved.");
    bool reg = g_regTiming.load();
    if (ImGui::Checkbox("Per-hook registry timing", &reg)) {
        g_regTiming.store(reg);
        g_regTimingActive.store(reg && g_enabled.load());
    }
    Hint("Time of each module's D3D9 registry hooks by name (draw calls; measured by framework/d3d9_hooks.cpp).");
    RenderKeyed();
    RenderSampling(save);
    if (save) ApexConfig::RequestSave();

    ImGui::SeparatorText("Hooks");
    {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        if (!g_summary.empty()) ImGui::TextDisabled("%s", g_summary.c_str());
        if (ImGui::BeginTable("##FpHooks", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Function");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Calls R / S / O");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();
            for (int i = 0; i < kTargetCount; i++) {
                const int c = kTargetCat[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kTargets[i].name);
                Hint(kCats[c].hint);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", static_cast<unsigned>(DisplayAddress(i)));
                ImGui::TableNextColumn();
                uint64_t calls[3];
                for (int b = 0; b < 3; b++) // FUN_00611680 returns after the Present boundary, so its calls are booked as Present (driver)
                    calls[b] = g_stats.bucketCalls[b][c] + (i == T_EndFramePresent ? g_stats.bucketCalls[b][kPresentDriver] : 0);
                ImGui::Text("%llu / %llu / %llu", static_cast<unsigned long long>(calls[0]), static_cast<unsigned long long>(calls[1]), static_cast<unsigned long long>(calls[2]));
                Hint("Calls since Clear on the render thread / simulation thread / other threads.");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g_targets[i].status.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::TextDisabled("GC call site: %s", GcCallSiteText().c_str());
    ImGui::TextDisabled("Frame limiter: %s", LimiterText().c_str());
    ImGui::TextDisabled("Clock: %s", ClockText().c_str());
    ImGui::TextDisabled("Not measurable through the D3D9 hook registry: the duration of resource creation (hooks run before the call; counts only),\n"
                        "CreateVertexBuffer / CreateIndexBuffer, Lock / Unlock, SetRenderState, DrawPrimitiveUP / DrawIndexedPrimitiveUP.");
    if (g_writerRunning.load() || g_qWritten.load() || g_qDropped.load())
        ImGui::TextDisabled("ApexRadiance_Hitches.txt: %u hitches written, %u dropped (queue full)", g_qWritten.load(), g_qDropped.load());

    if (ImGui::TreeNode("Totals since Clear")) {
        if (ImGui::BeginTable("##FpTotals", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Render s");
            ImGui::TableSetupColumn("Simulation s");
            ImGui::TableSetupColumn("Other s");
            ImGui::TableSetupColumn("Calls");
            ImGui::TableHeadersRow();
            for (int c = 0; c < kCatCount; c++) {
                const uint64_t calls = g_stats.bucketCalls[0][c] + g_stats.bucketCalls[1][c] + g_stats.bucketCalls[2][c];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kCats[c].name);
                Hint(kCats[c].hint);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g_stats.render[c] / 1000.0);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g_stats.sim[c] / 1000.0);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g_stats.other[c] / 1000.0);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(calls));
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Unattributed (render)");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", g_stats.unattributed / 1000.0);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Lots promoted %llu, demoted %llu", static_cast<unsigned long long>(g_stats.lotsPromoted), static_cast<unsigned long long>(g_stats.lotsDemoted));
        ImGui::TreePop();
    }

    if (g_regTiming.load()) {
        ImGui::SeparatorText("Registry hooks by name");
        if (g_regDisplay.empty()) {
            ImGui::TextDisabled("No data: d3d9_hook_registry.cpp is not instrumented (see frame_profiler.h).");
        } else {
            for (const auto& d : g_regDisplay) ImGui::Text("%-28s %.3f ms / frame   %.0f calls / frame", d.name.c_str(), d.msPerFrame, d.callsPerFrame);
        }
    }
    ImGui::TreePop();
}

} // namespace

namespace FrameProfiler {

void SetEnabled(bool on) {
    std::lock_guard<std::mutex> lk(g_ctrlMutex);
    if (on == g_enabled.load()) return;
    if (on) {
        std::call_once(g_clockOnce, InitClock);
        g_needBaseline.store(true);
        g_summary = "Waiting for the first frame to attach the timed functions";
        StartWriter(); // before the Present hook exists: the queue has no producer yet
        g_attachPending.store(true);
        RegisterD3DHooks(); // the first Present attaches the game functions (OnPresentStart)
        g_enabled.store(true);
        g_regTimingActive.store(g_regTiming.load());
        UpdateSamplerLocked();
        LOG_INFO("[FrameProfiler] On");
    } else {
        g_enabled.store(false);
        g_attachPending.store(false);
        g_regTimingActive.store(false);
        UpdateSamplerLocked(); // stops it: g_enabled is false
        D3D9Hooks::UnregisterAll(kHookName);
        for (int i = 0; i < kTargetCount; i++) DetachTarget(i);
        StopWriter();
        g_summary = "Off";
        LOG_INFO("[FrameProfiler] Off");
    }
}

bool IsEnabled() {
    return g_enabled.load();
}

void Shutdown() {
    SetEnabled(false);
}

void RenderUI(bool showEnable) {
    bool on = g_enabled.load();
    if (showEnable) { // the Violet menu's card has its own switch
        if (ImGui::Checkbox("Enable frame profiler", &on)) {
            SetEnabled(on);
            ApexConfig::RequestSave();
        }
        Hint("Measures every frame and records what each hitch is made of: lot streaming and LOD, lot lighting, terrain,\n"
             "script GC, the game's render and Present, the frame limiter and the mod's own D3D9 hooks.\n"
             "Off = nothing is hooked, no cost.");
    }
    if (on || g_stats.frames) {
        if (showEnable) ImGui::SameLine();
        if (ImGui::Button("Clear##FrameProfiler")) Clear();
        Hint("Forget the collected frames and hitches (ApexRadiance_Hitches.txt keeps what was written).");
        ImGui::SameLine();
        if (ImGui::Button("Save report now")) SaveReport();
        Hint("Append a full report (percentiles, totals, the last hitches, hook status) to ApexRadiance_Hitches.txt.");
    }
    if (!g_stats.frames) {
        ImGui::TextDisabled("%s", on ? "Waiting for frames..." : "Off: nothing is hooked.");
    } else {
        if (!on) ImGui::TextDisabled("Off: showing the data collected so far.");
        RenderLive();
        RenderHitches();
    }
    RenderAdvanced();
}

void SaveToToml(toml::table& qolTable) {
    toml::table t;
    t.insert("enabled", g_enabled.load());
    t.insert("hitch_multiplier", static_cast<double>(g_mult.load()));
    t.insert("hitch_floor_ms", static_cast<double>(g_floorMs.load()));
    t.insert("count_state_calls", g_countState.load());
    t.insert("write_file", g_writeFile.load());
    t.insert("sample_render", g_sampleRender.load());
    t.insert("sample_simulation", g_sampleSim.load());
    t.insert("sample_hz", static_cast<int64_t>(g_sampleHz.load()));
    qolTable.insert("frame_profiler", std::move(t));
}

void LoadFromToml(const toml::table& qolTable) {
    bool enabled = false;
    if (auto node = qolTable["frame_profiler"].as_table()) {
        const auto& t = *node;
        g_mult.store(std::clamp(static_cast<float>(t["hitch_multiplier"].value_or(2.0)), 1.2f, 5.0f));
        g_floorMs.store(std::clamp(static_cast<float>(t["hitch_floor_ms"].value_or(8.0)), 1.0f, 100.0f));
        const bool state = t["count_state_calls"].value_or(true);
        g_writeFile.store(t["write_file"].value_or(true));
        g_sampleRender.store(t["sample_render"].value_or(false));
        g_sampleSim.store(t["sample_simulation"].value_or(false));
        g_sampleHz.store(std::clamp(static_cast<int>(t["sample_hz"].value_or(int64_t{2000})), 250, 4000));
        enabled = t["enabled"].value_or(false);
        if (state != g_countState.load()) {
            g_countState.store(state);
            std::lock_guard<std::mutex> lk(g_ctrlMutex);
            if (g_enabled.load()) {
                D3D9Hooks::UnregisterAll(kHookName);
                RegisterD3DHooks();
            }
        }
    }
    SetEnabled(enabled);
    std::lock_guard<std::mutex> lk(g_ctrlMutex); // sampling options may have changed while the profiler stays on
    UpdateSamplerLocked();
}

bool RegistryHookTimingActive() {
    return g_regTimingActive.load(std::memory_order_relaxed);
}

uint64_t Ticks() {
    return Now();
}

// Keyed by the name string's address (fast path), verified by content: the registry's short names live inside its
// vectors' elements, so an address can be reused by another module's hook after a register / unregister.
void AddRegistryHookTime(const std::string& hookName, uint64_t ticks) {
    const char* p = hookName.c_str();
    RegPtr* cached = nullptr;
    for (auto& e : g_regPtrs) {
        if (e.ptr == p) {
            if (g_regNames[e.idx].name == hookName) {
                g_regNames[e.idx].ticks += ticks;
                g_regNames[e.idx].calls++;
                return;
            }
            cached = &e;
            break;
        }
    }
    int idx = -1;
    for (size_t i = 0; i < g_regNames.size(); i++)
        if (g_regNames[i].name == hookName) idx = static_cast<int>(i);
    if (idx < 0) {
        if (g_regNames.size() >= 64) return;
        g_regNames.push_back({hookName, 0, 0});
        idx = static_cast<int>(g_regNames.size()) - 1;
    }
    if (cached) cached->idx = idx;
    else if (g_regPtrs.size() < 256) g_regPtrs.push_back({p, idx});
    g_regNames[idx].ticks += ticks;
    g_regNames[idx].calls++;
}

} // namespace FrameProfiler

#endif // S3SS_PUBLIC
