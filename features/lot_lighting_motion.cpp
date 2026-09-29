// Lot lighting while the camera moves (see lot_lighting_motion.h and docs/features/performance.md).
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; addresses through framework/game_addresses.h) ----
//   0x00ADB8F0 lot lighting update, thiscall(lot lighting manager). Returns at once unless [this+0x18] and not
//              [this+0x4E]. Starts an EA stopwatch in ms (0x004F35B0(4, 0) + 0x00408700), then
//   0x00ADB95D   call 0x00ADB120 (mov ecx,esi): the budget, left in ST0; "fst [esp+0Ch]" keeps a copy for the loop test
//              and ST0 stays loaded; for each level object of the lot (a deque at this+0x24..0x40): 0x006A8BA0(stopwatch,
//              budget) (the call 0x00ADB9AD), then elapsed = 0x004F33C0; stop when elapsed > budget. So at least one level
//              runs every frame, and the budget bounds the rest.
//   0x006A8BA0 per level: its two light solvers (vfunc +0xC of this+0x290 / +0x2E8) and the dirty rooms (0x006A88B0 ->
//              ... -> 0x006A3C90), all with (stopwatch, budget). 0x006A3C90 is a resumable state machine (state at
//              room+0xEC): while room+0x164 is set it stops as soon as elapsed >= budget and resumes next frame; rooms
//              without +0x164 finish in one go. Nothing is skipped by a smaller budget: the work is spread.
//   0x00ADB120 budget, thiscall, ret, ST0 (only caller 0x00ADB95D): WorldManager [0x011ECBC4] +0x1B4 == 0 (tool mode):
//              [0x011833DC] = 1000; else lot id = [[this+0x14]+0x48/+0x4C] and 0x006FDC80(SceneObjectManager, id) (the id
//              is one of the two "priority" lot ids at SceneObjectManager +0x10D0 / +0x10E0, copied from +0x10D8 / +0x10E8
//              in 0x00701270; most likely the active / focused lot, not proven): [this+0x4F] (loading) ? [0x011833D8] = 30
//              : [0x00F9A62C] = 15; other lots: loading ? [0x01045CCC] = 10 : [0x00FBD498] = 5 (values read in TS3W.exe).
//   Callers: the lot renderer update 0x00AEB2E0 -> 0x00AE4CB0 -> 0x00ADB8F0 (render thread, per lot, every frame), also
//   reached from the lot impostor builder (0x00AD9E30 -> 0x00AEB3F0).
//   Camera: WorldManager::Update 0x00C6D5BD "call 0x006E8330 (mov eax,[0x011D1860]; ret); mov ecx,eax; call 0x006E8400
//   (mov eax,[ecx+24h]; ret); movaps xmm0,[eax+60h]" = the camera eye (its y is compared with the terrain height
//   threshold WorldManager+0xE8). Parsed from the code on every build (root global, camera offset, eye offset).
//
// ---- The patch ----
//   The 5 bytes of the CALL at 0x00ADB95D point to Hook_LotLightBudget (written with every other thread suspended and none
//   inside them; restored the same way). The hook calls 0x00ADB120 and returns, in ST0 (a float return), either the
//   game's value or, while the camera moves, max(0.25, min(game, game x budgetMs / 15)). Budgets >= 100 ms (tool mode)
//   are never changed. No other code is touched: the Frame Profiler's lot lighting targets (the entry of 0x00ADB8F0 and
//   the CALL 0x00ADB9AD) are other bytes, and it keeps timing (the room solves receive the scaled budget).
//
// Part of Apex Radiance. Credits: @loinyx

#include "lot_lighting_motion.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace LotLightingMotion {
namespace {

constexpr float kPriorityMs = 15.0f;       // the game's budget for a priority lot (0x00ADB120, constant 0x00F9A62C)
constexpr float kLeaveAloneMs = 100.0f;    // budgets from here up (the tool mode's 1000) are the game's
constexpr float kMinMs = 0.25f;            // never below
constexpr uint64_t kHoldMs = 300;          // "moving" lasts this long after the last camera change
constexpr float kMoveEpsilon = 0.005f;     // metres; smaller eye changes are not motion

using BudgetFn = float(__fastcall*)(void* mgr, void* edx);

std::mutex g_ctrl;
bool g_started = false;
uintptr_t g_call = 0;     // the CALL (0x00ADB95D on Steam)
uintptr_t g_budgetFn = 0; // 0x00ADB120 on Steam (constant while installed: the hook calls it)
uint8_t g_origCall[5] = {};
std::atomic<bool> g_on{false};
std::atomic<int> g_budgetMs{3};

// Camera eye: [[g_rootGlobal] + g_camOff] + g_eyeOff
uintptr_t g_rootGlobal = 0;
uint32_t g_camOff = 0, g_eyeOff = 0;
float g_lastEye[3] = {}; // written under g_sampling
bool g_haveEye = false;
std::atomic_flag g_sampling; // clear (C++20 default)
std::atomic<uint64_t> g_lastMoveTick{0};

// statistics
std::atomic<uint32_t> c_calls{0}, c_scaled{0}, c_eyeFails{0};
std::atomic<uint32_t> g_lastGame{0}, g_lastOut{0}; // float bits

bool ReadEye(float out[3]) {
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(g_rootGlobal);
        if (!root) return false;
        const uintptr_t camera = *reinterpret_cast<const uintptr_t*>(root + g_camOff);
        if (!camera) return false;
        const float* e = reinterpret_cast<const float*>(camera + g_eyeOff);
        out[0] = e[0];
        out[1] = e[1];
        out[2] = e[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void SampleCamera(uint64_t now) {
    if (g_sampling.test_and_set(std::memory_order_acquire)) return; // another thread is sampling
    float e[3];
    if (ReadEye(e)) {
        if (g_haveEye && (std::fabs(e[0] - g_lastEye[0]) > kMoveEpsilon || std::fabs(e[1] - g_lastEye[1]) > kMoveEpsilon || std::fabs(e[2] - g_lastEye[2]) > kMoveEpsilon))
            g_lastMoveTick.store(now, std::memory_order_relaxed);
        std::memcpy(g_lastEye, e, sizeof e);
        g_haveEye = true;
    } else {
        c_eyeFails.fetch_add(1, std::memory_order_relaxed);
        g_haveEye = false;
    }
    g_sampling.clear(std::memory_order_release);
}

bool MovingAt(uint64_t now) {
    const uint64_t last = g_lastMoveTick.load(std::memory_order_relaxed);
    return last && now - last <= kHoldMs;
}

// The lot lighting update's budget call (thiscall, ret, ST0): a float return is ST0 in every x86 convention
float __fastcall Hook_LotLightBudget(void* mgr, void* edx) {
    const float game = reinterpret_cast<BudgetFn>(g_budgetFn)(mgr, edx);
    if (!g_on.load(std::memory_order_acquire)) return game;
    c_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    SampleCamera(now);
    float out = game;
    if (MovingAt(now) && game > 0.0f && game < kLeaveAloneMs) {
        const float scaled = game * (static_cast<float>(g_budgetMs.load(std::memory_order_relaxed)) / kPriorityMs);
        out = std::max(kMinMs, std::min(game, scaled));
        if (out < game) c_scaled.fetch_add(1, std::memory_order_relaxed);
    }
    g_lastGame.store(std::bit_cast<uint32_t>(game), std::memory_order_relaxed);
    g_lastOut.store(std::bit_cast<uint32_t>(out), std::memory_order_relaxed);
    return out;
}

// "A1 imm32 C3" -> imm32
bool ParseRootGetter(uintptr_t fn, uintptr_t& global) {
    uint8_t b[6];
    if (!fn || !MemPatch::ReadBytes(fn, b, sizeof b) || b[0] != 0xA1 || b[5] != 0xC3) return false;
    uint32_t v;
    std::memcpy(&v, b + 1, 4);
    global = v;
    return v != 0;
}

// "8B 41 disp8 C3" -> disp8
bool ParseCameraGetter(uintptr_t fn, uint32_t& off) {
    uint8_t b[4];
    if (!fn || !MemPatch::ReadBytes(fn, b, sizeof b) || b[0] != 0x8B || b[1] != 0x41 || b[3] != 0xC3) return false;
    off = b[2];
    return true;
}

// the instruction after the camera getter's CALL: "0F 28 40 disp8" (movaps xmm0,[eax+disp8]) -> disp8
bool ParseEyeRead(uintptr_t call, uint32_t& off) {
    uint8_t b[4];
    if (!call || !MemPatch::ReadBytes(call + 5, b, sizeof b) || b[0] != 0x0F || b[1] != 0x28 || b[2] != 0x40) return false;
    off = b[3];
    return true;
}

uintptr_t CallTargetOf(const uint8_t call[5], uintptr_t at) {
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    return at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

float Bits(uint32_t v) { return std::bit_cast<float>(v); }

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("LotLightingMotion", &missing)) return fail(GameAddr::NotAvailable(missing));
    const uintptr_t call = GameAddr::Get(GameAddr::Id::LotLightBudgetCall);
    const uintptr_t budget = GameAddr::Get(GameAddr::Id::LotLightBudget);
    // Camera eye, from the code that reads it
    uintptr_t root = 0;
    uint32_t camOff = 0, eyeOff = 0;
    if (!ParseRootGetter(GameAddr::Get(GameAddr::Id::CameraRootGetter), root) || !ParseCameraGetter(GameAddr::Get(GameAddr::Id::CameraGetter), camOff) ||
        !ParseEyeRead(GameAddr::Get(GameAddr::Id::CameraGetterCall), eyeOff))
        return fail("The camera position was not found in the game's code");
    // The CALL must still reach the budget function, and its result must be read from ST0 (fst / fstp right after it)
    uint8_t cur[6] = {};
    if (!MemPatch::ReadBytes(call, cur, sizeof cur) || cur[0] != 0xE8 || CallTargetOf(cur, call) != budget)
        return fail(std::format("The lot lighting budget call at {:#010x} was changed by another module", call));
    if (cur[5] != 0xD9 && cur[5] != 0xDD) return fail(std::format("Unexpected code after the lot lighting budget call at {:#010x}", call));
    g_budgetFn = budget;
    g_rootGlobal = root;
    g_camOff = camOff;
    g_eyeOff = eyeOff;
    g_haveEye = false;
    g_lastMoveTick.store(0);
    std::memcpy(g_origCall, cur, 5);
    uint8_t bytes[5] = {0xE8};
    const int32_t rel = MemPatch::CalculateRelativeOffset(call, reinterpret_cast<uintptr_t>(&Hook_LotLightBudget));
    std::memcpy(bytes + 1, &rel, 4);
    g_on.store(true, std::memory_order_release); // before the CALL can reach the hook
    if (!MemPatch::WriteCodeSuspended(call, bytes, sizeof bytes)) {
        g_on.store(false);
        return fail(std::format("Could not patch the lot lighting budget call at {:#010x} (a thread kept running it, or the write failed)", call));
    }
    g_call = call;
    g_started = true;
    LOG_INFO(std::format("[LotLightingMotion] On: the call at {:#010x} to the lot lighting budget {:#010x} goes through Apex; camera eye [[{:#010x}]+{:#x}]+{:#x}; "
                         "budget while moving {} ms for the current lot",
                         call, budget, root, camOff, eyeOff, g_budgetMs.load()));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the hook returns the game's budget from now on
    uint8_t cur[5] = {};
    if (MemPatch::ReadBytes(g_call, cur, sizeof cur) && cur[0] == 0xE8 && CallTargetOf(cur, g_call) == reinterpret_cast<uintptr_t>(&Hook_LotLightBudget)) {
        if (!MemPatch::WriteCodeSuspended(g_call, g_origCall, sizeof g_origCall)) {
            LOG_ERROR(std::format("[LotLightingMotion] Could not restore the call at {:#010x}; it stays on Apex's wrapper, which now returns the game's budget", g_call));
            return; // g_started stays true: a later Stop retries
        }
    } else {
        LOG_WARNING(std::format("[LotLightingMotion] The call at {:#010x} was changed by another module after Apex; left as it is", g_call));
    }
    g_started = false;
    LOG_INFO(std::format("[LotLightingMotion] Off ({} budget calls, {} scaled while the camera moved)", c_calls.load(), c_scaled.load()));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetBudgetMs(int ms) { g_budgetMs.store(std::clamp(ms, 1, 15)); }
int BudgetMs() { return g_budgetMs.load(); }

bool CameraMoving() { return Running() && MovingAt(GetTickCount64()); }

std::string StatusText() {
    if (!Running()) return "Off";
    if (!c_calls.load()) return "On (no lot lighting work yet)";
    return CameraMoving() ? std::format("Camera moving: the current lot's lights get {} ms per frame", g_budgetMs.load()) : std::string("Camera still: the game's own lot lighting budget");
}

void RenderDeveloperUI() {
    if constexpr (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    ImGui::TextUnformatted(("Lot lighting while moving: " + StatusText()).c_str());
    const uint64_t last = g_lastMoveTick.load();
    const uint64_t now = GetTickCount64();
    ImGui::TextDisabled("Call %#010x -> %#010x; camera eye [[%#010x]+0x%X]+0x%X (%s); last camera move %s", static_cast<unsigned>(g_call), static_cast<unsigned>(g_budgetFn),
                        static_cast<unsigned>(g_rootGlobal), g_camOff, g_eyeOff, g_haveEye ? "read" : "not readable now",
                        last ? std::format("{:.1f} s ago", static_cast<double>(now - last) / 1000.0).c_str() : "never");
    ImGui::TextDisabled("Budget calls %u, scaled while moving %u, camera reads failed %u; last budget: game %.2f ms -> %.2f ms", c_calls.load(), c_scaled.load(), c_eyeFails.load(),
                        Bits(g_lastGame.load()), Bits(g_lastOut.load()));
}

} // namespace LotLightingMotion
