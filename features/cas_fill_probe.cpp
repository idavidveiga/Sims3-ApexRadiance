// CAS fill probe (see cas_fill_probe.h). Development build only.
//
// Part of Apex Radiance. Credits: @loinyx
#include "cas_fill_probe.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include <windows.h>
#include <detours/detours.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace CasFillProbe {
namespace {

// FUN_005d1010: 13 stack arguments, ret 34h; ECX set by the caller (thiscall) but not read. Arguments used here: a3 the
// vertex stride (u16), a4 the destination; the return value is a4 + stride x vertex count.
constexpr uintptr_t kFill = 0x005D1010;
// push ebp; mov ebp,esp; and esp,-16; sub esp,0D4h; push ebx; push esi; mov esi,[ebp+8]; mov eax,[esi]; mov edx,[eax+34h]
constexpr uint8_t kFillHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xD4, 0x00, 0x00, 0x00, 0x53, 0x56, 0x8B, 0x75, 0x08, 0x8B, 0x06, 0x8B, 0x50, 0x34};

using FillFn = uint32_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                     uint32_t, uint32_t, uint32_t);
void* g_orig = nullptr;
bool g_started = false;
double g_qpcMs = 0.0;

std::atomic<uint64_t> c_calls{0}, c_ticks{0}, c_verts{0}, c_bytes{0}, c_wc{0}, c_nocache{0}, c_private{0}, c_mapped{0}, c_image{0}, c_other{0};
std::atomic<uint64_t> g_maxTicks{0};
std::atomic<uint32_t> g_maxVerts{0}, g_maxStride{0};
std::atomic<uint32_t> g_maxProtect{0}, g_maxType{0};
std::atomic<int> g_slowLogged{0};
uint64_t g_lastCalls = 0;
DWORD g_lastLog = 0;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

uint32_t __fastcall Hook_Fill(void* ecx, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
                              uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13) {
    const uint64_t t0 = Qpc();
    const uint32_t r = reinterpret_cast<FillFn>(g_orig)(ecx, edx, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    const uint64_t dt = Qpc() - t0;
    const uint32_t stride = a3 & 0xFFFF;
    const uint32_t verts = stride && r >= a4 ? (r - a4) / stride : 0;
    MEMORY_BASIC_INFORMATION mbi{};
    DWORD protect = 0, type = 0;
    if (a4 && VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(a4)), &mbi, sizeof mbi)) {
        protect = mbi.Protect;
        type = mbi.Type;
    }
    c_calls.fetch_add(1, std::memory_order_relaxed);
    c_ticks.fetch_add(dt, std::memory_order_relaxed);
    c_verts.fetch_add(verts, std::memory_order_relaxed);
    c_bytes.fetch_add(static_cast<uint64_t>(verts) * stride, std::memory_order_relaxed);
    if (protect & PAGE_WRITECOMBINE) c_wc.fetch_add(1, std::memory_order_relaxed);
    if (protect & PAGE_NOCACHE) c_nocache.fetch_add(1, std::memory_order_relaxed);
    (type == MEM_PRIVATE ? c_private : type == MEM_MAPPED ? c_mapped : type == MEM_IMAGE ? c_image : c_other).fetch_add(1, std::memory_order_relaxed);
    if (dt > g_maxTicks.load(std::memory_order_relaxed)) {
        g_maxTicks.store(dt, std::memory_order_relaxed);
        g_maxVerts.store(verts, std::memory_order_relaxed);
        g_maxStride.store(stride, std::memory_order_relaxed);
        g_maxProtect.store(protect, std::memory_order_relaxed);
        g_maxType.store(type, std::memory_order_relaxed);
    }
    const double ms = static_cast<double>(dt) * g_qpcMs;
    if (ms >= 5.0 && g_slowLogged.fetch_add(1, std::memory_order_relaxed) < 40)
        LOG_INFO(std::format("[CasFillProbe] Slow call: {:.2f} ms, {} vertices x {} bytes to {:#010x} (protect {:#x}, type {:#x}, region base {:#010x} size {} KB)", ms, verts,
                             stride, a4, protect, type, reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize / 1024));
    return r;
}

} // namespace

void Start() {
    if constexpr (kPublicBuild) return;
    if (g_started) return;
    g_started = true;
    if (!GameAddr::IsFixed()) {
        LOG_INFO("[CasFillProbe] Not started: Steam 1.67.2 only");
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(kFill), kFillHead, sizeof kFillHead) != 0) {
        LOG_WARNING(std::format("[CasFillProbe] Not started: the bytes at {:#010x} differ", kFill));
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    g_orig = reinterpret_cast<void*>(kFill);
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(&g_orig, reinterpret_cast<void*>(&Hook_Fill)) != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
        DetourTransactionAbort();
        g_orig = nullptr;
        LOG_WARNING("[CasFillProbe] Not started: Detours failed");
        return;
    }
    LOG_INFO(std::format("[CasFillProbe] On: FUN_005d1010 (CAS/ModelBuilder/FillDrawable's vertex packing) is timed; a summary every 10 s while it runs"));
}

void Tick() {
    if constexpr (kPublicBuild) return;
    if (!g_orig) return;
    const DWORD now = GetTickCount();
    if (now - g_lastLog < 10000) return;
    const uint64_t calls = c_calls.load();
    if (calls == g_lastCalls) return;
    g_lastLog = now;
    g_lastCalls = calls;
    LOG_INFO(std::format("[CasFillProbe] {} calls, {:.1f} ms in all, {} vertices, {:.1f} MB written; destination: write-combined {}, uncached {}, private {}, mapped {}, "
                         "image {}, other {}; slowest {:.2f} ms ({} vertices x {} bytes, protect {:#x}, type {:#x})",
                         calls, static_cast<double>(c_ticks.load()) * g_qpcMs, c_verts.load(), static_cast<double>(c_bytes.load()) / 1048576.0, c_wc.load(), c_nocache.load(),
                         c_private.load(), c_mapped.load(), c_image.load(), c_other.load(), static_cast<double>(g_maxTicks.load()) * g_qpcMs, g_maxVerts.load(),
                         g_maxStride.load(), g_maxProtect.load(), g_maxType.load()));
}

} // namespace CasFillProbe
