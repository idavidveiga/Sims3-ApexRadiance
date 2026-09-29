#pragma once
// Layered hooks on game functions that are reached only through vtable slots (no code byte changes).
//
// Several Apex modules may wrap the same function; each wrapper is a "layer" with a fixed position (lower = outer),
// whatever order the modules install in. The game's vtable slots always hold the outermost installed layer's hook; a
// layer calls Next(site, layer), which is the next inner installed layer's hook, or the game function itself.
//   - Install: the new layer's next pointer is set first, then either the slots are swapped to it (it becomes the
//     outermost: one interlocked compare-exchange per slot, while the page is writable) or the next outer layer's next
//     pointer is re-pointed to it (one atomic store).
//   - Remove: the reverse. The removed layer keeps its next pointer, so a thread still inside it finishes normally; hooks
//     and the game function stay in memory for the process lifetime.
//   - Every slot must hold the expected pointer (the game function, or the current outermost Apex layer) before
//     anything is written; a slot changed by another module makes Install fail and is never overwritten by Remove.
// Addresses come from the game-address table (framework/game_addresses.h). Used by the Frame Profiler (dev build) and
// the resource lookup cache (features/resource_cache.h) on ResourceMgr::FindProvider and GetKeyList, by the cache alone on
// the resource manager's database-list methods, by the Frame Profiler and the fast RefPack compressor
// (features/fast_refpack.h) on the RefPack stream write, and by the wall shading gate (features/lot_lighting_motion.h)
// and the Frame Profiler on the wall AO step. Thread-safe (Install / Remove serialise on one mutex; Next is
// lock-free). Code-entry hooks shared the same way: framework/entry_chain.h.
#include <atomic>
#include <string>

namespace SlotChain {

enum class Site : int {
    FindProvider,      // ResourceMgr::FindProvider 0x004AFFC0, slots +0x40 of both resource manager vtables
    RegisterDb,        // ResourceMgr::RegisterDatabase 0x004B2D00, slot +0x34 of the base vtable 0x00FB2DA0
    RegisterDbDerived, // the ResourceSystem override 0x00736A70, slot +0x34 of the derived vtable 0x00FFE250
    SetDbPriority,     // 0x004B2EC0, slots +0x3C of both vtables
    DbChanged,         // 0x004B0960, slots +0x4C of both vtables
    RefPackCompress,   // RefPack stream write 0x004EC200, slot +4 of the stream vtable 0x00FB9018 (0x00FB901C)
    WallAoStep,        // wall ambient-occlusion solver step 0x0068B810, slot +0x1C of the solver vtable 0x00FF0594 (0x00FF05B0)
    KeyListBase,       // ResourceMgr::GetKeyList 0x004B1AE0, slot +0x20 of the base vtable 0x00FB2DA0 (0x00FB2DC0)
    KeyListDerived,    // ResourceSystem::GetKeyList 0x00736660 (calls 0x004B1AE0 directly), slot +0x20 of the derived vtable (0x00FFE270)
    Count
};
// lower = outer. Gate: the wall shading gate (features/lot_lighting_motion.h), on the WallAoStep site only; it must be
// outermost because it recognises its caller by its own return address. FastCompress: the fast RefPack compressor
// (features/fast_refpack.h), on the RefPackCompress site only. ResourceCache: the lookup cache, the key list cache and
// the package-list watchers (features/resource_cache.h).
enum class Layer : int { Gate, FrameProfiler, ResourceCache, FastCompress, Count };

// Installs `hook` as `layer` of `site` (true when installed, or already installed). error: why not.
bool Install(Site site, Layer layer, void* hook, std::string* error);
// Removes the layer (nothing when it is not installed).
void Remove(Site site, Layer layer);
bool Installed(Site site, Layer layer);
// The game function of the site (0 until the first Install resolved it)
uintptr_t GameFunction(Site site);

// What `layer`'s hook must call: never null while the layer is installed (and afterwards, for threads still inside it).
extern std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)];
inline void* Next(Site site, Layer layer) { return g_next[static_cast<int>(site)][static_cast<int>(layer)].load(std::memory_order_acquire); }

} // namespace SlotChain
