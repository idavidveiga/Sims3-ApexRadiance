#pragma once
// Who already hooks a function: reads its first instruction and, for a jump, names the module it lands in. Logged at
// every Apex attach, so the order of the Detours chains with other mods (official S3SS, ReShade, overlays) is visible in
// ApexRadiance_LOG.txt.
#include <windows.h>
#include <string>

namespace HookChain {

// "clean (8B FF 55 8B EC)", "E9 -> Sims3SettingsSetter.asi+0x1234", "FF 25 -> d3d9.dll+0x40", ...
std::string DescribePrologue(const void* function);
// Module file name (no path) containing the address, or "?".
std::string ModuleOf(const void* address);
// "module+0xoffset"
std::string AddressText(const void* address);

} // namespace HookChain
