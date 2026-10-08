// TS3 EA 1.69 ships an activation-protected on-disk PE whose .text entropy is
// effectively 8 bits/byte. An unverified disk signature is NOT an ABI proof.
// This is a manual, read-only in-process diagnostic run after normal game startup.
// No hooks, no writes, no method swapping, no external modding libraries.
#include "ts3_mono_runtime_probe.h"
#include "apex_log.h"
#include "game_version.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace Ts3MonoRuntimeProbe {
namespace {
std::string g_status =
    "Not inspected. Run this only after the world has finished loading.";
constexpr size_t kSampleBytes = 65536;
constexpr char kPriorSteamResolver[] =
    "81 EC 08 08 00 00 53 55 8B AC 24 14 08";

std::string InspectLoadedExe() {
    auto* image = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    if (!image) return "No loaded main executable module";
    const auto module = reinterpret_cast<uintptr_t>(image);

    IMAGE_DOS_HEADER dos{};
    if (!MemPatch::ReadBytes(module, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 0x4000)
        return "Invalid live DOS header";

    IMAGE_NT_HEADERS32 nt{};
    const auto ntAt = module + static_cast<uint32_t>(dos.e_lfanew);
    if (!MemPatch::ReadBytes(ntAt, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.NumberOfSections > 32)
        return "Invalid live PE32 headers";

    const auto sectionAt = ntAt + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                           nt.FileHeader.SizeOfOptionalHeader;
    IMAGE_SECTION_HEADER text{};
    bool found = false;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER candidate{};
        if (!MemPatch::ReadBytes(sectionAt + i * sizeof(candidate),
                                 &candidate, sizeof(candidate)))
            return "Cannot read live section headers";
        if (std::memcmp(candidate.Name, ".text", 5) == 0) {
            text = candidate;
            found = true;
            break;
        }
    }
    if (!found || !text.Misc.VirtualSize || text.VirtualAddress >= nt.OptionalHeader.SizeOfImage)
        return "No valid executable .text section";

    const size_t length = std::min<size_t>(
        text.Misc.VirtualSize,
        nt.OptionalHeader.SizeOfImage - text.VirtualAddress);
    const size_t sample = std::min(length, kSampleBytes);
    std::vector<BYTE> bytes(sample);
    const auto textAt = module + text.VirtualAddress;
    if (!MemPatch::ReadBytes(textAt, bytes.data(), bytes.size()))
        return "Could not safely read loaded .text memory";

    std::array<size_t, 256> frequencies{};
    for (const auto byte : bytes) ++frequencies[byte];
    double bits = 0.0;
    for (const auto count : frequencies) {
        if (!count) continue;
        const double p = static_cast<double>(count) / bytes.size();
        bits -= p * std::log2(p);
    }

    // For protected, still-high-entropy memory, scanning for the Steam
    // signature has no diagnostic value. Never identify such bytes as code.
    if (bits >= 7.95) {
        return std::format(
            "Loaded .text remains high-entropy ({:.3f} bits/byte; sampled {} bytes). "
            "A native Mono resolver/JIT entry cannot be verified from this observation. "
            "No hooks installed.", bits, sample);
    }

    // Observation only. A matching historical Steam prologue is NOT evidence
    // of method identity, calling convention, JIT state, or compatibility.
    const uintptr_t match = MemPatch::ScanPattern(
        image + text.VirtualAddress, length, kPriorSteamResolver);
    if (match) {
        return std::format(
            "Loaded .text entropy {:.3f}; historical Steam-shaped prologue at RVA {:#x}. "
            "Candidate UNVERIFIED: no native hook installed or approved.", bits,
            match - module);
    }
    return std::format(
        "Loaded .text entropy {:.3f} ({}-byte sample); historical Steam prologue "
        "not found. Native EA Mono API/ABI remain unverified. No hooks installed.",
        bits, sample);
}
} // namespace

void RenderDeveloperUI() {
    ImGui::TextUnformatted("CAS/CASt native Mono research (read-only)");
    ImGui::TextWrapped("Game build: %s. No Mono hook is installed by this probe.",
                       GetGameVersionName());
    if (ImGui::Button("Inspect loaded TS3.exe (read-only)")) {
        g_status = InspectLoadedExe();
        LOG_INFO(std::format("[TS3 Mono Runtime Probe] {}", g_status));
    }
    ImGui::TextWrapped("%s", g_status.c_str());
    ImGui::TextDisabled("A matching signature alone never validates a JIT hook or its x86 ABI.");
}
} // namespace Ts3MonoRuntimeProbe
