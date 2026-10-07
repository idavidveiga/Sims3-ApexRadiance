// Exception safety nets (see hook_guard.h).
#include "hook_guard.h"
#include "apex_log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <format>

namespace HookGuard {
namespace {

// One slot per distinct place. state: 0 free, 1 being written, 2 ready (name complete). Written by the noting thread,
// read by the render thread at Present; `logged` belongs to that reader.
struct Slot {
    std::atomic<int> state{0};
    char where[160] = {};
    std::atomic<long> count{0};
    long logged = 0;
};
constexpr int kSlots = 48;
Slot g_slots[kSlots];
std::atomic<bool> g_pending{false};
std::atomic<long> g_lost{0}; // notes that found no free slot
long g_lostLogged = 0;

} // namespace

void Note(const char* where) noexcept {
    if (!where) where = "?";
    for (Slot& s : g_slots) {
        int st = s.state.load(std::memory_order_acquire);
        if (st == 0) {
            int expected = 0;
            if (s.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                strncpy_s(s.where, where, _TRUNCATE);
                s.count.fetch_add(1, std::memory_order_relaxed);
                s.state.store(2, std::memory_order_release);
                g_pending.store(true, std::memory_order_release);
                return;
            }
            st = expected;
        }
        if (st == 2 && std::strncmp(s.where, where, sizeof s.where - 1) == 0) {
            s.count.fetch_add(1, std::memory_order_relaxed);
            g_pending.store(true, std::memory_order_release);
            return;
        }
    }
    g_lost.fetch_add(1, std::memory_order_relaxed);
    g_pending.store(true, std::memory_order_release);
}

void NoteAt(const char* where, const void* code) noexcept {
    char buf[160];
    HMODULE module = nullptr;
    char name[MAX_PATH] = {};
    if (code && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(code), &module) &&
        module && GetModuleFileNameA(module, name, MAX_PATH)) {
        const char* base = name;
        for (const char* p = name; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        std::snprintf(buf, sizeof buf, "%s (code at %s+0x%X)", where ? where : "?", base,
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(code) - reinterpret_cast<uintptr_t>(module)));
    } else {
        std::snprintf(buf, sizeof buf, "%s (code at %p)", where ? where : "?", code);
    }
    Note(buf);
}

void ReportPending() noexcept {
    if (!g_pending.exchange(false, std::memory_order_acq_rel)) return;
    try {
        for (Slot& s : g_slots) {
            if (s.state.load(std::memory_order_acquire) != 2) continue;
            const long c = s.count.load(std::memory_order_relaxed);
            if (c == s.logged) continue;
            s.logged = c;
            LOG_ERROR(std::format("[HookGuard] {} threw a C++ exception ({} so far): caught; the game went on as it does without Apex there, and that "
                                  "step stays off until the game restarts",
                                  s.where, c));
        }
        const long lost = g_lost.load(std::memory_order_relaxed);
        if (lost != g_lostLogged) {
            g_lostLogged = lost;
            LOG_ERROR(std::format("[HookGuard] {} more C++ exceptions caught in other places (no room left to name them)", lost));
        }
    } catch (...) {
        g_pending.store(true, std::memory_order_relaxed); // try again next frame
    }
}

} // namespace HookGuard
