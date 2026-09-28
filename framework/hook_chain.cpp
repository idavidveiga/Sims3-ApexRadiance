#include "hook_chain.h"
#include "memory_patch.h"
#include <cstdio>
#include <cstring>

namespace HookChain {

std::string ModuleOf(const void* address) {
    HMODULE mod = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &mod) || !mod)
        return "?";
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(mod, path, MAX_PATH)) return "?";
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

std::string AddressText(const void* address) {
    HMODULE mod = nullptr;
    char buf[64];
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &mod) && mod) {
        std::snprintf(buf, sizeof buf, "+0x%X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mod)));
        return ModuleOf(address) + buf;
    }
    std::snprintf(buf, sizeof buf, "0x%08X (no module)", static_cast<unsigned>(reinterpret_cast<uintptr_t>(address)));
    return buf;
}

std::string DescribePrologue(const void* function) {
    BYTE b[8] = {};
    const uintptr_t at = reinterpret_cast<uintptr_t>(function);
    if (!function || !MemPatch::ReadBytes(at, b, sizeof b)) return "unreadable";
    char hex[40];
    std::snprintf(hex, sizeof hex, "%02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4]);
    if (b[0] == 0xE9) { // jmp rel32
        int32_t rel;
        std::memcpy(&rel, b + 1, 4);
        return "E9 -> " + AddressText(reinterpret_cast<const void*>(at + 5 + rel));
    }
    if (b[0] == 0xEB) { // jmp rel8 (often a hot-patch stub before the function)
        return std::string("EB (short jump, ") + hex + ")";
    }
    if (b[0] == 0xFF && b[1] == 0x25) { // jmp [abs32]
        uintptr_t slot;
        std::memcpy(&slot, b + 2, 4);
        uintptr_t dest = 0;
        if (MemPatch::ReadBytes(slot, &dest, sizeof dest)) return "FF 25 -> " + AddressText(reinterpret_cast<const void*>(dest));
        return "FF 25 (unreadable slot)";
    }
    if (b[0] == 0x68 && b[5] == 0xC3) { // push imm32; ret
        uintptr_t dest;
        std::memcpy(&dest, b + 1, 4);
        return "push/ret -> " + AddressText(reinterpret_cast<const void*>(dest));
    }
    return std::string("clean (") + hex + ")";
}

} // namespace HookChain
