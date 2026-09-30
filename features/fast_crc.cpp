// Faster record checksums: FUN_004fa4c0 answered eight bytes per step (see fast_crc.h and docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_crc.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "build_flavor.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace FastCrc {
namespace {

using EntryChain::Layer;
using EntryChain::Site;
// cdecl(bytes, length, crc, bool invert): the bool is a 32-bit push whose low byte the game tests
using CrcFn = uint32_t(__cdecl*)(const uint8_t*, uint32_t, uint32_t, uint32_t);

constexpr uint32_t kStartupChecks = 16;
constexpr uint32_t kDevCheckEvery = 64; // development build: 1 call in N after the startup checks

uint32_t g_t[8][256]; // g_t[0] = the game's table; g_t[k][i] = g_t[k-1][i] advanced by one zero byte (written before the hook goes in)

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
std::atomic<uint32_t> g_seq{0};
std::atomic<uint64_t> c_calls{0}, c_bytes{0}, c_checked{0}, c_mismatch{0};

// The game's loop, one table lookup per byte (the reference the fast path is compared with offline and at start)
uint32_t ByteWise(const uint8_t* p, uint32_t len, uint32_t crc) {
    for (uint32_t i = 0; i < len; i++) crc = (crc << 8) ^ g_t[0][(crc >> 24) ^ p[i]];
    return crc;
}

uint32_t Sliced(const uint8_t* p, uint32_t len, uint32_t crc) {
    while (len >= 8) {
        const uint32_t c = crc ^ (static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]);
        crc = g_t[7][c >> 24] ^ g_t[6][(c >> 16) & 0xFF] ^ g_t[5][(c >> 8) & 0xFF] ^ g_t[4][c & 0xFF] ^ g_t[3][p[4]] ^ g_t[2][p[5]] ^ g_t[1][p[6]] ^
              g_t[0][p[7]];
        p += 8;
        len -= 8;
    }
    return ByteWise(p, len, crc);
}

// The game's semantics around the loop: nothing is read when bytes + length wraps (its `jae` on the end pointer), and the
// result is inverted when the low byte of the fourth argument is set
uint32_t Compute(const uint8_t* p, uint32_t len, uint32_t crc, uint32_t invert) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a + len > a) crc = Sliced(p, len, crc);
    return (invert & 0xFF) ? ~crc : crc;
}

// No C++ objects (SEH)
bool ReadTable(uintptr_t at, uint32_t* out) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), 256 * sizeof(uint32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool BuildTables(uintptr_t table, std::string* why) {
    if (!ReadTable(table, g_t[0])) {
        *why = std::format("its table {:#010x} could not be read", table);
        return false;
    }
    // A CRC table is linear: T[0] = 0 and every entry the XOR of the entries of its bits. Only then do the derived tables
    // give the byte-wise result.
    if (g_t[0][0] != 0) {
        *why = "its table does not start with 0";
        return false;
    }
    for (uint32_t i = 1; i < 256; i++) {
        uint32_t x = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1u << b)) x ^= g_t[0][1u << b];
        if (x != g_t[0][i]) {
            *why = std::format("its table is not a CRC table (entry {})", i);
            return false;
        }
    }
    for (int k = 1; k < 8; k++)
        for (int i = 0; i < 256; i++) g_t[k][i] = (g_t[k - 1][i] << 8) ^ g_t[0][g_t[k - 1][i] >> 24];
    return true;
}

// Test buffers through the game's own function (not hooked yet) and through Apex's
bool SelfTest(CrcFn game, std::string* why) {
    std::vector<uint8_t> buf(70000 + 16);
    uint32_t s = 0x9E3779B9u;
    for (auto& b : buf) {
        s = s * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(s >> 24);
    }
    static const uint32_t kLens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 63, 64, 65, 100, 255, 256, 1000, 4096, 4099, 65536, 70000};
    int n = 0;
    for (uint32_t len : kLens)
        for (uint32_t off = 0; off < 8; off++) {
            s = s * 1664525u + 1013904223u;
            const uint32_t seed = (n & 3) == 0 ? 0xFFFFFFFFu : (n & 3) == 1 ? 0u : s;
            const uint32_t inv = n & 1;
            n++;
            const uint8_t* p = buf.data() + off;
            const uint32_t want = game(p, len, seed, inv);
            const uint32_t got = Compute(p, len, seed, inv);
            if (want != got) {
                *why = std::format("test {} ({} bytes at +{}): the game gives {:#010x}, Apex {:#010x}", n, len, off, want, got);
                return false;
            }
        }
    return true;
}

uint32_t __cdecl Hook_Crc(const uint8_t* p, uint32_t len, uint32_t crc, uint32_t invert) {
    const CrcFn game = reinterpret_cast<CrcFn>(EntryChain::Next(Site::RecordCrc, Layer::FastCrc));
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed)) return game(p, len, crc, invert);
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    const uint32_t r = Compute(p, len, crc, invert);
    c_calls.fetch_add(1, std::memory_order_relaxed);
    c_bytes.fetch_add(len, std::memory_order_relaxed);
    const bool check = seq < kStartupChecks || (!kPublicBuild && seq % kDevCheckEvery == 0);
    if (!check) return r;
    const uint32_t want = game(p, len, crc, invert);
    c_checked.fetch_add(1, std::memory_order_relaxed);
    if (want == r) return r;
    c_mismatch.fetch_add(1, std::memory_order_relaxed);
    if (!g_selfDisabled.exchange(true))
        LOG_ERROR(std::format("[FastCrc] Result differs from the game's ({} bytes: the game {:#010x}, Apex {:#010x}). The game's value was used; record checksums "
                              "go back to the game's code for this session.",
                              len, want, r));
    return want;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("FastRecordCrc", &missing)) return fail(GameAddr::NotAvailable(missing));
    const uintptr_t fn = GameAddr::Get(GameAddr::Id::RecordCrc);
    std::string why;
    if (!BuildTables(GameAddr::Get(GameAddr::Id::RecordCrcTable), &why)) return fail("The game's record checksum was not recognised: " + why);
    // Before the first hook the entry is the game's own code; once hooked (re-start after Stop), the trampoline
    const CrcFn game = reinterpret_cast<CrcFn>(EntryChain::Original(Site::RecordCrc) ? EntryChain::Original(Site::RecordCrc) : reinterpret_cast<void*>(fn));
    if (!SelfTest(game, &why)) return fail("The record checksum gave a different value than the game's: " + why);
    std::string err;
    if (!EntryChain::Install(Site::RecordCrc, Layer::FastCrc, reinterpret_cast<void*>(&Hook_Crc), &err)) return fail("Could not hook the record checksum: " + err);
    g_selfDisabled.store(false);
    g_seq.store(0);
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[FastCrc] On: the record checksum {:#010x} answered eight bytes per step (test buffers equal to the game's); the first {} calls are "
                         "checked against the game",
                         fn, kStartupChecks));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release);
    const bool removed = EntryChain::Remove(Site::RecordCrc, Layer::FastCrc);
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastCrc] Off ({} calls, {:.1f} MB, {} checked, {} different){}", s.calls, static_cast<double>(s.bytes) / (1024.0 * 1024.0), s.checked,
                         s.mismatches, removed ? "" : "; the entry could not be put back (the hook stays and passes every call through)"));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

Stats GetStats() {
    Stats s;
    s.calls = c_calls.load();
    s.bytes = c_bytes.load();
    s.checked = c_checked.load();
    s.mismatches = c_mismatch.load();
    s.selfDisabled = g_selfDisabled.load();
    return s;
}

std::string StatusText() {
    if (!Running()) return "Record checksums: off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Record checksums: turned themselves off (a value differed from the game's, see ApexRadiance_LOG.txt)";
    return std::format("Record checksums: {} ({:.1f} MB), {} checked against the game, all equal", s.calls, static_cast<double>(s.bytes) / (1024.0 * 1024.0), s.checked);
}

} // namespace FastCrc
