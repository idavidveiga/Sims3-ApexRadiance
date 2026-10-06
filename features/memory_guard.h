#pragma once
// Room to save (Apex Radiance, feature "MemoryGuard"; Performance page, Memory handling card).
//
// TS3W.exe is 32-bit: what runs out in a long session is not memory but one large free block of its 4 GB address space.
// "Error 12" is the world save failing (0x00AAC110 returns 12 at 0x00AAC334 when the world save 0x00C6D460 returns
// false); it writes large streams, and a big allocation needs one contiguous free block (memory study 30/09, Backups
// Sims 3\400). This keeps room for it, with nothing visible changed:
//  - a reserve of address space (only reserved: no memory, no commit) taken at start and let go right before each world
//    save, and whenever the largest free block of the process gets small; taken again when there is room once more;
//  - the game's own resource cache (idle files kept up to about 200 MB, "Resources/CacheBudget") emptied through the
//    game's own shrink (ResourceSystem vtable +0x50, 0x00733E70: both caches, idle entries only) right before each save
//    and when the largest free block gets small. Only idle resources go; the game reads them again when it needs them.
// Steam 1.67.2: fixed addresses, every site checked byte for byte; other builds (EA 1.69): found by signature (GameAddr).
#include <string>

namespace MemoryGuard {

bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();
void RenderDeveloperUI();

} // namespace MemoryGuard
