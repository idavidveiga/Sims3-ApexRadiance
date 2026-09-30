// Address-space monitor (development build; see address_space.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "address_space.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <vector>

namespace AddressSpace {
namespace {

constexpr DWORD kPeriodMs = 10000;
constexpr int kLogEvery = 6; // snapshots (one line a minute)
constexpr int kClasses = 6;
const char* const kClassNames[kClasses] = {"< 64 KB", "64 KB - 1 MB", "1 - 4 MB", "4 - 16 MB", "16 - 64 MB", ">= 64 MB"};

struct Block {
    uintptr_t base = 0;
    uint64_t size = 0;
};
struct Image {
    uintptr_t base = 0;
    uint64_t size = 0;
    std::string name;
};
struct Snapshot {
    std::string when;
    uint64_t free = 0, freeLow = 0, freeHigh = 0, largest = 0, largestLow = 0, largestHigh = 0;
    Block top[5];
    uint64_t committed[3] = {}, reserved[3] = {}; // image, mapped, private: committed / reserved-only
    uint64_t privateExec = 0;                     // committed private PAGE_EXECUTE_READWRITE (the script GC heap)
    uint32_t regions = 0, allocations = 0;
    uint32_t classCount[kClasses] = {};
    uint64_t classBytes[kClasses] = {};
    uint32_t gameBigBlocks = 0;
    uint64_t gameBigBytes = 0;
    bool haveGame = false;
    std::vector<Image> images; // largest first, top 8
};

std::mutex g_lock;
Snapshot g_last, g_min; // g_min: the snapshot with the smallest largest free block
bool g_haveLast = false;
HANDLE g_thread = nullptr, g_stop = nullptr;

int ClassOf(uint64_t size) {
    constexpr uint64_t kLim[kClasses - 1] = {64ull << 10, 1ull << 20, 4ull << 20, 16ull << 20, 64ull << 20};
    int c = 0;
    while (c < kClasses - 1 && size >= kLim[c]) c++;
    return c;
}

bool ReadU32(uintptr_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string NowText() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return std::format("{:02}:{:02}:{:02}", t.wHour, t.wMinute, t.wSecond);
}

Snapshot Take() {
    Snapshot s;
    s.when = NowText();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress), hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<Block> frees;
    std::vector<Image> images;
    uintptr_t allocBase = 0, allocType = 0;
    uint64_t allocSize = 0;
    auto closeAlloc = [&] {
        if (!allocBase) return;
        s.allocations++;
        if (allocType == MEM_PRIVATE) {
            const int c = ClassOf(allocSize);
            s.classCount[c]++;
            s.classBytes[c] += allocSize;
        } else if (allocType == MEM_IMAGE) {
            images.push_back({allocBase, allocSize, {}});
        }
        allocBase = 0;
        allocSize = 0;
    };
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = lo; a < hi && VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof m) == sizeof m;) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
        const uintptr_t size = m.RegionSize;
        s.regions++;
        if (m.State == MEM_FREE) {
            closeAlloc();
            s.free += size;
            const bool low = base < 0x80000000u;
            (low ? s.freeLow : s.freeHigh) += size;
            uint64_t& l = low ? s.largestLow : s.largestHigh;
            l = std::max<uint64_t>(l, size);
            s.largest = std::max<uint64_t>(s.largest, size);
            frees.push_back({base, size});
        } else {
            const uintptr_t ab = reinterpret_cast<uintptr_t>(m.AllocationBase);
            if (ab != allocBase) {
                closeAlloc();
                allocBase = ab;
                allocType = m.Type;
            }
            allocSize += size;
            const int k = m.Type == MEM_IMAGE ? 0 : m.Type == MEM_MAPPED ? 1 : 2;
            if (m.State == MEM_COMMIT) {
                s.committed[k] += size;
                if (k == 2 && m.Protect == PAGE_EXECUTE_READWRITE) s.privateExec += size;
            } else {
                s.reserved[k] += size;
            }
        }
        if (base + size <= a) break;
        a = base + size;
    }
    closeAlloc();
    std::sort(frees.begin(), frees.end(), [](const Block& x, const Block& y) { return x.size > y.size; });
    for (size_t i = 0; i < 5 && i < frees.size(); i++) s.top[i] = frees[i];
    std::sort(images.begin(), images.end(), [](const Image& x, const Image& y) { return x.size > y.size; });
    if (images.size() > 8) images.resize(8);
    for (Image& im : images) {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(im.base), path, MAX_PATH)) {
            const wchar_t* n = wcsrchr(path, L'\\');
            for (const wchar_t* c = n ? n + 1 : path; *c; c++) im.name += *c < 128 ? static_cast<char>(*c) : '?';
        } else {
            im.name = std::format("{:08X}", im.base);
        }
    }
    s.images = std::move(images);
    // the game allocator's own VirtualAlloc'd big blocks (128 KB and more)
    if (GameAddr::Resolved()) {
        uint32_t alloc = 0, n = 0, bytes = 0;
        if (const uintptr_t g = GameAddr::Get(GameAddr::Id::AllocGlobal); g && ReadU32(g, &alloc) && alloc && ReadU32(alloc + 0x488, &n) && ReadU32(alloc + 0x48C, &bytes)) {
            s.haveGame = true;
            s.gameBigBlocks = n;
            s.gameBigBytes = bytes;
        }
    }
    return s;
}

constexpr double MB(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

std::string LineOf(const Snapshot& s) {
    return std::format("free {:.0f} MB (below 2 GB {:.0f}, above {:.0f}), largest free block {:.0f} MB (below 2 GB {:.0f}, above {:.0f}); committed: images {:.0f}, "
                       "mapped {:.0f}, private {:.0f} MB (script heap / executable {:.0f}); reserved only: {:.0f} MB; {} allocations",
                       MB(s.free), MB(s.freeLow), MB(s.freeHigh), MB(s.largest), MB(s.largestLow), MB(s.largestHigh), MB(s.committed[0]), MB(s.committed[1]),
                       MB(s.committed[2]), MB(s.privateExec), MB(s.reserved[0] + s.reserved[1] + s.reserved[2]), s.allocations);
}

std::string TableOf(const Snapshot& s) {
    std::string t = "   " + LineOf(s) + "\n   largest free blocks:";
    for (const Block& b : s.top)
        if (b.size) t += std::format(" {:.0f} MB at {:08X};", MB(b.size), b.base);
    t += "\n   private allocations by size:";
    for (int c = 0; c < kClasses; c++)
        if (s.classCount[c]) t += std::format(" {} x{} = {:.0f} MB;", kClassNames[c], s.classCount[c], MB(s.classBytes[c]));
    t += "\n   largest images:";
    for (const Image& im : s.images) t += std::format(" {} {:.1f} MB;", im.name, MB(im.size));
    if (s.haveGame) t += std::format("\n   game allocator big blocks (own VirtualAlloc, 128 KB and more): {} = {:.0f} MB", s.gameBigBlocks, MB(s.gameBigBytes));
    return t + "\n";
}

DWORD WINAPI Proc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int n = 0;
    do {
        Snapshot s = Take();
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (!g_haveLast || s.largest < g_min.largest) g_min = s;
            g_last = s;
            g_haveLast = true;
        }
        if (n++ % kLogEvery == 0) LOG_INFO("[AddressSpace] " + LineOf(s));
    } while (WaitForSingleObject(g_stop, kPeriodMs) == WAIT_TIMEOUT);
    return 0;
}

} // namespace

void Start() {
    if constexpr (kPublicBuild) return;
    if (g_thread) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop) g_thread = CreateThread(nullptr, 64 * 1024, Proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void Stop() {
    if (!g_thread) return;
    SetEvent(g_stop); // not waited for: called under the loader lock, which a thread's exit needs
    CloseHandle(g_thread);
    g_thread = nullptr;
}

std::string ReportText() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_haveLast) return "";
    return std::format("Address space (VirtualQuery every 10 s; the game is 32-bit, 4 GB):\n   latest ({}):\n{}   session minimum of the largest free block ({}):\n{}", g_last.when,
                       TableOf(g_last), g_min.when, TableOf(g_min));
}

} // namespace AddressSpace
