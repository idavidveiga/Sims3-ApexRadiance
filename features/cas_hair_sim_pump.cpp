// Experimental Apex-only simulator-thread execution point for CAS Hair/Hats.
//
// This is the actual native dispatch edge between an EA MonoScriptHost
// ProcessTasks call and the existing HairNativeBridge::OnSimulationTick.
// Installing it requires independent EA 1.69 ABI verification and an
// explicitly compiled opt-in macro; ordinary builds never install this hook.
//
// Important: this does NOT replace CASHair.PopulateTypesGrid. A separate,
// verified interpreter method adapter must call Session().Begin and Arm()
// before the pump has anything to execute. No unsafe managed calls or
// speculative method replacement are installed here.
#include "cas_hair_sim_pump.h"
#include "ts3_cas_mono_sites.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "memory_patch.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>

namespace ApexCasHairSimPump {
namespace {
using ProcessTasksFn = void(__thiscall*)(void*);

ApexCasSchedule::HairNativeBridge g_session;
std::recursive_mutex g_hookMutex;
std::atomic<bool> g_running{false};
std::atomic<bool> g_threadConflict{false};
std::atomic<DWORD> g_simThread{0};
std::atomic<Generation> g_armed{0};
std::atomic<std::uint64_t> g_calls{0}, g_slices{0}, g_refused{0};
ProcessTasksFn g_original = nullptr;

#if defined(APEX_ENABLE_EXPERIMENTAL_CAS_MINT_PUMP) && defined(_M_IX86)
// __fastcall accepts the game's ECX receiver; the dummy EDX parameter
// maintains the original x86 __thiscall stack shape.
// This wrapper MUST NOT be enabled before confirming the actual EA
// ProcessTasks return type, arguments, owner and post-call lifetime.
void __fastcall HookProcessTasks(void* host, void*) {
    const ProcessTasksFn original = g_original;
    if (!original) return;
    original(host); // the original game's task dispatch executes first
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_running.load(std::memory_order_acquire) ||
        g_threadConflict.load(std::memory_order_acquire)) return;

    const DWORD current = GetCurrentThreadId();
    DWORD unknown = 0;
    if (!g_simThread.compare_exchange_strong(
            unknown, current, std::memory_order_acq_rel) &&
        unknown != current) {
        g_threadConflict.store(true, std::memory_order_release);
        g_armed.store(0, std::memory_order_release);
        return;
    }
    Generation generation = g_armed.load(std::memory_order_acquire);
    if (!generation) return;

    // ProcessTasks can call code which synchronously re-enters it. No
    // nested scheduling: the original task pump is still called above.
    static thread_local bool inSlice = false;
    if (inSlice) return;
    inSlice = true;
    const bool safe = HookGuard::Try("CAS Hair/Hats simulation pump", [&] {
        const auto outcome = g_session.OnSimulationTick(generation);
        g_slices.fetch_add(1, std::memory_order_relaxed);
        if (outcome.wrongThread || outcome.refused ||
            outcome.canceled || outcome.needsOriginalRebuild ||
            outcome.slice.completed || outcome.slice.aborted) {
            g_armed.compare_exchange_strong(
                generation, 0,
                std::memory_order_acq_rel);
        }
        if (outcome.wrongThread || outcome.refused)
            g_refused.fetch_add(1, std::memory_order_relaxed);
    });
    inSlice = false;
    if (!safe) {
        // A callback may have appended a partial cell before throwing.
        // Never retry it: a verified parent-method adapter must perform a
        // clean original-grid rebuild on the simulator thread.
        g_session.AbortAndRequestOriginalRebuild();
        g_armed.store(0, std::memory_order_release);
    }
}
#endif

bool ExpectedProcessTasksEntry(std::uintptr_t entry) {
    if (!entry) return false;
    const auto& pattern = ApexCasMono::kBridgePatterns[
        static_cast<std::size_t>(ApexCasMono::BridgeSite::ScriptHostProcessTasks)];
    if (pattern.length < 8 || pattern.length > 64) return false;
    std::array<std::uint8_t, 64> bytes{};
    if (!MemPatch::ReadBytes(entry, bytes.data(), pattern.length)) return false;
    return ApexCasMono::MatchesPattern(
        std::span<const std::uint8_t>(bytes.data(), pattern.length), 0, pattern);
}
} // namespace

ApexCasSchedule::HairNativeBridge& Session() noexcept { return g_session; }

bool Start(std::uintptr_t processTasksAddress,
           std::size_t verifiedSignatureMatches,
           bool nativeAbiIndependentlyVerified,
           std::string* error) {
#if !defined(APEX_ENABLE_EXPERIMENTAL_CAS_MINT_PUMP) || !defined(_M_IX86)
    (void)processTasksAddress;
    (void)verifiedSignatureMatches;
    (void)nativeAbiIndependentlyVerified;
    if (error) *error =
        "Hair/Hats MINT simulation pump unavailable: independent EA ABI "
        "validation and experimental Win32 compilation are required";
    return false;
#else
    std::lock_guard<std::recursive_mutex> guard(g_hookMutex);
    if (g_running.load(std::memory_order_acquire)) return true;
    if (!nativeAbiIndependentlyVerified || verifiedSignatureMatches != 1 ||
        !ExpectedProcessTasksEntry(processTasksAddress)) {
        if (error) *error =
            "EA MonoScriptHost::ProcessTasks ABI or unique original "
            "instruction entry was not validated; original CAS unchanged";
        return false;
    }
    g_original = reinterpret_cast<ProcessTasksFn>(processTasksAddress);
    g_simThread.store(0);
    g_threadConflict.store(false);
    g_armed.store(0);
    if (!DetourBatch::InstallHooks({
            {reinterpret_cast<void**>(&g_original),
             reinterpret_cast<void*>(&HookProcessTasks)}})) {
        if (error) *error =
            "Could not attach native ProcessTasks detour; original CAS unchanged";
        return false;
    }
    g_running.store(true, std::memory_order_release);
    LOG_INFO("[Apex CAS MINT] Experimental native simulation pump attached; "
             "no managed Hair/Hats method is replaced by this hook");
    return true;
#endif
}

void Disarm() noexcept { g_armed.store(0, std::memory_order_release); }

void Stop() {
    std::lock_guard<std::recursive_mutex> guard(g_hookMutex);
    Disarm();
#if defined(APEX_ENABLE_EXPERIMENTAL_CAS_MINT_PUMP) && defined(_M_IX86)
    if (!g_running.exchange(false)) return;
    if (!DetourBatch::RemoveHooks({
            {reinterpret_cast<void**>(&g_original),
             reinterpret_cast<void*>(&HookProcessTasks)}})) {
        LOG_WARNING("[Apex CAS MINT] ProcessTasks detach failed; "
                    "retained a disarmed original-call pass-through");
    }
#endif
}

bool Running() { return g_running.load(std::memory_order_acquire); }

bool Arm(Generation generation) noexcept {
    if (!generation || !Running() ||
        g_threadConflict.load(std::memory_order_acquire) ||
        g_simThread.load(std::memory_order_acquire) != GetCurrentThreadId() ||
        !g_session.Active() ||
        g_session.CurrentGeneration() != generation) {
        return false;
    }
    g_armed.store(generation, std::memory_order_release);
    return true;
}

std::string StatusText() {
    return std::format(
        "sim pump: {}; original calls={}; CAS slices={}; refusals={}; "
        "thread conflict={}; managed parent hook=not installed",
        Running() ? "attached (experimental)" : "off",
        g_calls.load(), g_slices.load(), g_refused.load(),
        g_threadConflict.load() ? "yes" : "no");
}
} // namespace ApexCasHairSimPump
