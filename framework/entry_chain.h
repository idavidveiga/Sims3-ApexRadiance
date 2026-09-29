#pragma once
// Layered hooks on the ENTRY of game functions that several Apex modules wrap (the code-entry twin of slot_chain.h).
//
// The entry's first instructions (a verified relocation-free prologue) are copied to a trampoline (+ JMP back), and a
// 5-byte JMP to the outermost installed layer's hook is written over them. Each layer calls Next(site, layer): the next
// inner installed layer's hook, or the trampoline (= the game's own function). Layers have fixed positions (lower =
// outer), whatever order the modules install in:
//   - Install: the new layer's next pointer first; then either the entry JMP is (re)written to it (it becomes the
//     outermost) or the next outer layer's next pointer is re-pointed to it (one atomic store).
//   - Remove: the reverse; when the last layer goes, the original prologue bytes are written back.
//   - Code bytes are written only with every other thread suspended and none of them stopped inside the bytes
//     (MemPatch::WriteCodeSuspended): several threads call these functions. A removed hook keeps its next pointer, so a
//     thread still inside it finishes normally; hooks and trampolines stay in memory for the process lifetime.
//   - Before the first write the entry must hold the expected prologue; later, the JMP Apex wrote. Anything else
//     (another module hooked it) makes Install fail and is never overwritten by Remove.
// Used by the Frame Profiler (dev build, the outer layer) and the fast DXT encoder (features/fast_dxt.h) on the game's
// CPU DXT1 / DXT5 encoders. Thread-safe (Install / Remove serialise on one mutex; Next is lock-free).
#include <atomic>
#include <cstdint>
#include <string>

namespace EntryChain {

enum class Site : int {
    DxtEncode1, // 0x006152F0 cdecl(Dst*, Src*), prologue 55 8B EC 83 E4 F0 (push ebp; mov ebp,esp; and esp,-16)
    DxtEncode5, // 0x006154B0, same prologue
    Count
};
enum class Layer : int { FrameProfiler, FastDxt, Count }; // lower = outer

// Installs `hook` as `layer` of `site` (true when installed, or already installed). error: why not.
bool Install(Site site, Layer layer, void* hook, std::string* error);
// Removes the layer (nothing when it is not installed). False only when the original bytes could not be written back.
bool Remove(Site site, Layer layer);
bool Installed(Site site, Layer layer);
// The function's entry (0 until the first Install resolved it)
uintptr_t GameFunction(Site site);
// A callable copy of the game's function (the trampoline), or nullptr before the first Install
void* Original(Site site);

// What `layer`'s hook must call: never null while the layer is installed (and afterwards, for threads still inside it).
extern std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)];
inline void* Next(Site site, Layer layer) { return g_next[static_cast<int>(site)][static_cast<int>(layer)].load(std::memory_order_acquire); }

} // namespace EntryChain
