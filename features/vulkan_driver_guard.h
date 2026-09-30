#pragma once
// One graphics driver in the game (docs/features/performance.md, "How it works: the unused Vulkan driver").
//
// DXVK asks the Vulkan loader for every GPU, so on a PC with an NVIDIA (or Intel) card and an AMD integrated GPU the
// loader also loads AMD's 32-bit Vulkan driver (amdvlk32.dll, 85 MB of code mapped in the middle of the game's low 2 GB)
// into TS3W, though the game renders on the other card. On a 32-bit game that space is what Error 12 runs out of (the
// address-space study of 30/09, loadre\addrspace.md). Right before the game's first Direct3DCreate9 (the loader is not
// loaded yet), this sets VK_LOADER_DRIVERS_DISABLE for this process only, so the loader skips AMD's driver manifests,
// when all of these hold:
//   - vulkan-1.dll is not loaded yet, and no VK_LOADER_DRIVERS_* / VK_DRIVER_FILES / VK_ICD_FILENAMES variable is set
//     (the user's own choice wins);
//   - DXGI lists an AMD adapter and a non-AMD hardware adapter, and the adapter with the most dedicated video memory (the
//     one DXVK renders on) is not AMD, with every AMD adapter under half of its memory (an integrated GPU).
// Nothing is written outside the process; on a PC with only AMD, or an AMD card as the main one, nothing happens.
namespace VulkanDriverGuard {

// Game thread, before the real Direct3DCreate9. Once per process; logs what it decided.
void BeforeDirect3DCreate();
// For the log / Compatibility page: what was decided ("" before BeforeDirect3DCreate ran)
const char* Decision();

} // namespace VulkanDriverGuard
