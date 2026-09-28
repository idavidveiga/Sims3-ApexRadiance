// Map view state, read through the game's own script binding
//
// The script API registers its native calls from a table of {function, name} pairs in .data. The entry named
// "ScriptCore.CameraController::Camera_IsMapViewModeEnabled" (0x0073E060 on 1.67.2 Steam) takes no arguments, walks
// app -> world -> camera manager -> camera interface (cast id 0x110FDD89) and returns the byte at camera + 0x8B9, or
// false when any link is missing. It only reads, so calling it from the render thread is safe.
//
// The function is found through the name string and the table at run time (no fixed address), and its shape is
// validated before use: a getter that ends in "mov al, [eax + disp32]; ret; xor al, al; ret".

#include "map_view.h"
#include "apex_log.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>

namespace {

using IsMapViewFn = bool(__cdecl*)();

IsMapViewFn g_fn = nullptr;
std::once_flag g_once;

struct Section {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

bool FindSection(HMODULE module, const char* name, Section& out) {
    auto base = reinterpret_cast<uintptr_t>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (std::strncmp(reinterpret_cast<const char*>(s->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
            out.begin = base + s->VirtualAddress;
            out.end = out.begin + s->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

uintptr_t FindBytes(const Section& sec, const void* bytes, size_t size, size_t align) {
    for (uintptr_t p = sec.begin; p + size <= sec.end; p += align)
        if (std::memcmp(reinterpret_cast<const void*>(p), bytes, size) == 0) return p;
    return 0;
}

// The getter's epilogue within its first 0x60 bytes: 8A 80 xx xx xx xx C3 32 C0 C3, after the cast id push
bool LooksLikeGetter(const uint8_t* f) {
    if (f[0] != 0xE8) return false;
    bool castId = false;
    for (int i = 0; i < 0x50; i++) {
        if (f[i] == 0x68 && f[i + 1] == 0x89 && f[i + 2] == 0xDD && f[i + 3] == 0x0F && f[i + 4] == 0x11) castId = true;
        if (castId && f[i] == 0x8A && f[i + 1] == 0x80 && f[i + 6] == 0xC3 && f[i + 7] == 0x32 && f[i + 8] == 0xC0 && f[i + 9] == 0xC3)
            return true;
    }
    return false;
}

void Resolve() {
    HMODULE exe = GetModuleHandleW(nullptr);
    Section text, rdata;
    if (!FindSection(exe, ".text", text) || !FindSection(exe, ".rdata", rdata)) {
        LOG_WARNING("[MapView] Game sections not found");
        return;
    }
    static const char kName[] = "ScriptCore.CameraController::Camera_IsMapViewModeEnabled";
    const uintptr_t name = FindBytes(rdata, kName, sizeof(kName), 1); // includes the terminator
    if (!name) {
        LOG_WARNING("[MapView] Script binding name not found");
        return;
    }
    // the {function, name} table itself is initialised data: on 1.67.2 it sits in .data (0x0115DD20), not in .rdata
    const uint32_t nameVa = static_cast<uint32_t>(name);
    Section data;
    uintptr_t entry = 0;
    if (FindSection(exe, ".data", data)) entry = FindBytes(data, &nameVa, sizeof(nameVa), 4);
    if (!entry) entry = FindBytes(rdata, &nameVa, sizeof(nameVa), 4);
    if (!entry) { // (the function pointer before it is range- and shape-checked below)
        LOG_WARNING("[MapView] Script binding table entry not found");
        return;
    }
    const uintptr_t fn = *reinterpret_cast<const uint32_t*>(entry - 4);
    if (fn < text.begin || fn + 0x60 > text.end || !LooksLikeGetter(reinterpret_cast<const uint8_t*>(fn))) {
        LOG_WARNING(std::format("[MapView] Unexpected function at {:#010x}, map view detection off", fn));
        return;
    }
    g_fn = reinterpret_cast<IsMapViewFn>(fn);
    LOG_INFO(std::format("[MapView] Camera_IsMapViewModeEnabled at {:#010x}", fn));
}

bool CallGuarded(IsMapViewFn fn, bool& result) {
    __try {
        result = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

namespace MapView {

bool Available() {
    std::call_once(g_once, Resolve);
    return g_fn != nullptr;
}

bool IsOpen() {
    if (!Available()) return false;
    bool open = false;
    if (!CallGuarded(g_fn, open)) {
        LOG_WARNING("[MapView] Call faulted, map view detection off");
        g_fn = nullptr;
        return false;
    }
    return open;
}

} // namespace MapView
