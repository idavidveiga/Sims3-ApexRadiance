#include "memory_patch.h"
#include "apex_log.h"
#include <detours/detours.h>
#include <psapi.h>
#include <cstring>
#include <format>
#include <mutex>

namespace MemPatch {
namespace {

std::mutex g_writeLock; // one protected write at a time

// SEH wrappers (no C++ objects with destructors inside __try).
bool GuardedCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProtectedWrite(uintptr_t address, const BYTE* data, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok = GuardedCopy(reinterpret_cast<void*>(address), data, n);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), n);
    return ok;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "8B ?? 4E" -> bytes + mask (mask 0 = wildcard). Empty on a malformed pattern.
bool ParsePattern(const char* pattern, std::vector<BYTE>& bytes, std::vector<BYTE>& mask) {
    bytes.clear();
    mask.clear();
    for (const char* p = pattern; p && *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (*p == '?') {
            bytes.push_back(0);
            mask.push_back(0);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        const int hi = HexDigit(p[0]), lo = p[1] ? HexDigit(p[1]) : -1;
        if (hi < 0 || lo < 0) return false;
        bytes.push_back(static_cast<BYTE>(hi * 16 + lo));
        mask.push_back(1);
        p += 2;
    }
    return !bytes.empty();
}

// First start position in [from, to) where the pattern matches (it may read up to limit), or 0. A fault ends the search
// of this range (SEH: no C++ objects in here).
uintptr_t ScanRange(uintptr_t from, uintptr_t to, uintptr_t limit, const BYTE* bytes, const BYTE* mask, size_t n) {
    __try {
        for (uintptr_t p = from; p < to && p + n <= limit; p++) {
            const BYTE* at = reinterpret_cast<const BYTE*>(p);
            size_t k = 0;
            while (k < n && (!mask[k] || at[k] == bytes[k])) k++;
            if (k == n) return p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

} // namespace

bool ReadBytes(uintptr_t address, void* out, size_t count) {
    if (!address || !out) return false;
    return GuardedCopy(out, reinterpret_cast<const void*>(address), count);
}

bool ValidateBytes(LPCVOID address, const BYTE* expected, size_t count) {
    if (!address || !expected) return false;
    std::vector<BYTE> current(count);
    if (!ReadBytes(reinterpret_cast<uintptr_t>(address), current.data(), count)) return false;
    return std::memcmp(current.data(), expected, count) == 0;
}

bool WriteBytes(uintptr_t address, const std::vector<BYTE>& bytes, std::vector<PatchLocation>* undo, const std::vector<BYTE>* expected) {
    if (!address || bytes.empty()) return false;
    std::lock_guard<std::mutex> lock(g_writeLock);
    std::vector<BYTE> before(bytes.size());
    if (!ReadBytes(address, before.data(), before.size())) {
        LOG_ERROR(std::format("[MemPatch] {:#010x} is not readable", address));
        return false;
    }
    if (expected && !expected->empty()) {
        std::vector<BYTE> check(expected->size());
        if (!ReadBytes(address, check.data(), check.size()) || check != *expected) {
            LOG_ERROR(std::format("[MemPatch] {:#010x}: unexpected bytes, not patched", address));
            return false;
        }
    }
    if (!ProtectedWrite(address, bytes.data(), bytes.size())) {
        LOG_ERROR(std::format("[MemPatch] {:#010x}: write failed", address));
        return false;
    }
    if (undo) undo->push_back({address, std::move(before)});
    return true;
}

bool WriteDWORD(uintptr_t address, DWORD value, std::vector<PatchLocation>* undo, const DWORD* expected) {
    std::vector<BYTE> bytes(4);
    std::memcpy(bytes.data(), &value, 4);
    if (!expected) return WriteBytes(address, bytes, undo, nullptr);
    std::vector<BYTE> want(4);
    std::memcpy(want.data(), expected, 4);
    return WriteBytes(address, bytes, undo, &want);
}

bool RestoreAll(std::vector<PatchLocation>& undo) {
    std::lock_guard<std::mutex> lock(g_writeLock);
    bool ok = true;
    while (!undo.empty()) {
        const PatchLocation& last = undo.back();
        if (!ProtectedWrite(last.address, last.original.data(), last.original.size())) {
            LOG_ERROR(std::format("[MemPatch] Could not restore {} bytes at {:#010x}", last.original.size(), last.address));
            ok = false;
            break;
        }
        undo.pop_back();
    }
    return ok;
}

int32_t CalculateRelativeOffset(uintptr_t from, uintptr_t to, size_t length) {
    return static_cast<int32_t>(static_cast<intptr_t>(to) - static_cast<intptr_t>(from + length));
}

bool GetModuleInfo(HMODULE module, BYTE** base, size_t* size) {
    if (!module || !base || !size) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &mi, sizeof mi)) return false;
    *base = static_cast<BYTE*>(mi.lpBaseOfDll);
    *size = mi.SizeOfImage;
    return true;
}

uintptr_t ScanPattern(const BYTE* base, size_t size, const char* pattern) {
    std::vector<BYTE> bytes, mask;
    if (!base || !ParsePattern(pattern, bytes, mask) || size < bytes.size()) return 0;
    // Walk committed, readable regions only (an image can contain reserved or guard pages).
    const uintptr_t start = reinterpret_cast<uintptr_t>(base), end = start + size;
    uintptr_t at = start;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi)) break;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd > end) regionEnd = end;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS && prot != 0;
        if (readable) {
            // start positions are limited to this region; a match may read on into the next one (a fault ends the range)
            if (const uintptr_t hit = ScanRange(at, regionEnd, end, bytes.data(), mask.data(), bytes.size())) return hit;
        }
        if (regionEnd <= at) break;
        at = regionEnd;
    }
    return 0;
}

} // namespace MemPatch

namespace DetourBatch {

bool InstallHooks(const std::vector<Hook>& hooks) {
    if (hooks.empty()) return true;
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    for (const Hook& h : hooks) {
        const LONG r = DetourAttach(h.target, h.detour);
        if (r != NO_ERROR) {
            LOG_ERROR(std::format("[DetourBatch] DetourAttach failed ({})", r));
            DetourTransactionAbort();
            return false;
        }
    }
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) {
        LOG_ERROR(std::format("[DetourBatch] Commit failed ({})", r));
        return false;
    }
    return true;
}

bool RemoveHooks(const std::vector<Hook>& hooks) {
    if (hooks.empty()) return true;
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    for (const Hook& h : hooks) {
        const LONG r = DetourDetach(h.target, h.detour);
        if (r != NO_ERROR) {
            LOG_ERROR(std::format("[DetourBatch] DetourDetach failed ({})", r));
            DetourTransactionAbort();
            return false;
        }
    }
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) {
        LOG_ERROR(std::format("[DetourBatch] Commit failed ({})", r));
        return false;
    }
    return true;
}

} // namespace DetourBatch

std::optional<uintptr_t> GameAddress::Resolve() const {
    auto checked = [&](uintptr_t a) {
        return expectedBytes.empty() || MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(a), expectedBytes.data(), expectedBytes.size());
    };
    for (const PerVersion& v : addresses) {
        if (v.version != g_gameVersion) continue;
        if (checked(v.address)) return v.address;
        LOG_ERROR(std::format("[{}] {:#010x} does not hold the expected bytes on {}", name, v.address, GetGameVersionName()));
        return std::nullopt; // a known build with different bytes: something else changed it, do not guess
    }
    if (!pattern || !*pattern) {
        LOG_ERROR(std::format("[{}] No address for {} and no pattern", name, GetGameVersionName()));
        return std::nullopt;
    }
    BYTE* base = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size)) return std::nullopt;
    const uintptr_t hit = MemPatch::ScanPattern(base, size, pattern);
    if (!hit) {
        LOG_ERROR(std::format("[{}] Pattern not found", name));
        return std::nullopt;
    }
    const uintptr_t a = hit + static_cast<intptr_t>(patternOffset);
    if (!checked(a)) {
        LOG_ERROR(std::format("[{}] Pattern match {:#010x} does not hold the expected bytes", name, a));
        return std::nullopt;
    }
    LOG_INFO(std::format("[{}] Found by pattern at {:#010x}", name, a));
    return a;
}
