// Faster script math: the Mono interpreter's NaN tests done inline instead of calling msvcr80!_isnan (see script_math.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "script_math.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <vector>

namespace ScriptMath {
namespace {

std::atomic<bool> g_running{false};
uint32_t g_handlers = 0;

constexpr size_t kLenA = 22, kLenB = 13;
constexpr int kMaxHandlers = 64;

// block A with the slot and the esi step as wildcards (-1)
constexpr int kBlockA[kLenA] = {0x8B, 0x1D, -1, -1, -1, -1, 0x83, 0xEE, -1, 0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24, 0xFF, 0xD3, 0x83, 0xC4, 0x08, 0x85, 0xC0};
constexpr uint8_t kBlockB[kLenB] = {0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24, 0xFF, 0xD3, 0x83, 0xC4, 0x08, 0x85, 0xC0};

// The new code keeps the instructions from the old return address of `call ebx` on (A +17 "add esp, 8 ; test eax, eax",
// B +8): a thread inside _isnan while the code is rewritten returns into the same instructions. Only the bytes before
// it change, written with every other thread suspended outside them.
constexpr size_t kKeepA = 17, kKeepB = 8;
// A: mov ebx, _isnan ; sub esi, step ; xor eax, eax ; push eax ; push eax ; fucomip st0, st0 ; jnp +1 ; inc eax
//    (fucomip pops the operand; PF = 1 only when it is a NaN; the two pushes stand for the old "sub esp, 8")
// B: eax is 0 here (A's "jnz" fell through, and nothing jumps into B or onto the load before it):
//    push eax ; push eax ; fucomip st0, st0 ; jnp +1 ; inc eax ; nop
constexpr uint8_t kNewB[kKeepB] = {0x50, 0x50, 0xDF, 0xE8, 0x7B, 0x01, 0x40, 0x90};

struct Saved {
    uintptr_t a, b;
    uint8_t oldA[kKeepA], newA[kKeepA], oldB[kKeepB];
};
std::vector<Saved> g_saved;

bool WriteAll(bool on) {
    std::vector<MemPatch::CodeWrite> w;
    w.reserve(g_saved.size() * 2);
    for (const Saved& s : g_saved) {
        w.push_back({s.a, on ? s.newA : s.oldA, kKeepA, kKeepA});
        w.push_back({s.b, on ? kNewB : s.oldB, kKeepB, kKeepB});
    }
    return MemPatch::WriteCodeBatchSuspended(w.data(), w.size());
}

bool MatchA(const uint8_t* p) {
    for (size_t i = 0; i < kLenA; i++)
        if (kBlockA[i] >= 0 && p[i] != kBlockA[i]) return false;
    return true;
}

bool TextRange(const uint8_t*& begin, const uint8_t*& end) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (std::memcmp(s->Name, ".text", 6) == 0) {
            begin = base + s->VirtualAddress;
            end = begin + s->Misc.VirtualSize;
            return true;
        }
    return false;
}

struct Site {
    uintptr_t a, b;
    uint8_t step;
};

// SEH only (no C++ objects): every handler's blocks; the slot all of them read. -1 = unreadable, else how many.
int Scan(const uint8_t* begin, const uint8_t* end, Site* out, uint32_t* slotOut) {
    int n = 0;
    uint32_t slot = 0;
    __try {
        for (const uint8_t* p = begin; p + kLenA + 0x40 < end; p++) {
            if (p[0] != 0x8B || p[1] != 0x1D || !MatchA(p)) continue;
            uint32_t s;
            std::memcpy(&s, p + 2, 4);
            // the jump on the result, then the second operand's load
            const uint8_t* q = p + kLenA;
            if (q[0] == 0x0F && q[1] == 0x85) q += 6;
            else if (q[0] == 0x75) q += 2;
            else continue;
            if (q[0] == 0xDD && q[1] == 0x46) q += 3;
            else if (q[0] == 0xDD && q[1] == 0x06) q += 2;
            else continue;
            if (std::memcmp(q, kBlockB, kLenB) != 0) continue;
            if (slot && s != slot) return -2; // two different slots: not the code this was written for
            slot = s;
            if (n >= kMaxHandlers) return -3;
            out[n++] = {reinterpret_cast<uintptr_t>(p), reinterpret_cast<uintptr_t>(q), p[8]};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    *slotOut = slot;
    return n;
}

} // namespace

bool Start(std::string* error) {
    if (g_running.load()) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = "Faster script math: " + why;
        return false;
    };
    const HMODULE crt = GetModuleHandleW(L"msvcr80.dll");
    using FnIsNan = int(__cdecl*)(double);
    const auto isnan = crt ? reinterpret_cast<FnIsNan>(GetProcAddress(crt, "_isnan")) : nullptr;
    if (!isnan) return fail("the game's C runtime (msvcr80.dll) is not loaded");
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    if (isnan(nan) != 1 || isnan(-nan) != 1 || isnan(1.0) != 0 || isnan(inf) != 0 || isnan(-0.0) != 0)
        return fail("_isnan does not answer 0 / 1 as expected");
    const uint8_t *begin = nullptr, *end = nullptr;
    if (!TextRange(begin, end)) return fail("the game's code section was not found");
    Site sites[kMaxHandlers];
    uint32_t slot = 0;
    const int n = Scan(begin, end, sites, &slot);
    if (n <= 0) return fail(n == 0 ? "the interpreter's NaN tests were not found" : "the game code differs");
    uint32_t held = 0;
    if (!MemPatch::ReadBytes(slot, &held, 4) || held != reinterpret_cast<uint32_t>(isnan)) return fail("the import slot does not hold _isnan");
    g_saved.clear();
    for (int i = 0; i < n; i++) {
        const Site& s = sites[i];
        Saved v{s.a, s.b};
        if (!MemPatch::ReadBytes(s.a, v.oldA, kKeepA) || !MemPatch::ReadBytes(s.b, v.oldB, kKeepB)) return fail("the game code could not be read");
        const uint8_t a[kKeepA] = {0xBB, 0, 0, 0, 0, 0x83, 0xEE, s.step, 0x31, 0xC0, 0x50, 0x50, 0xDF, 0xE8, 0x7B, 0x01, 0x40};
        std::memcpy(v.newA, a, kKeepA);
        std::memcpy(v.newA + 1, &held, 4);
        g_saved.push_back(v);
    }
    if (!WriteAll(true)) {
        WriteAll(false); // a batch that failed half-way: put back what it wrote
        g_saved.clear();
        return fail("could not patch the game (a thread stayed inside the code)");
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_handlers = static_cast<uint32_t>(n);
    g_running = true;
    LOG_INFO(std::format("[ScriptMath] On: {} interpreter compare handlers test NaN inline ({} calls to _isnan removed; first {:#010x}, slot {:#010x})", n, n * 2,
                         sites[0].a, slot));
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    if (!WriteAll(false)) LOG_ERROR("[ScriptMath] The game code could not be put back (a thread stayed inside it)");
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
}

bool Running() { return g_running.load(); }

std::string StatusText() {
    return g_running.load() ? std::format("on ({} script compare handlers test NaN without a call)", g_handlers) : "off";
}

} // namespace ScriptMath
