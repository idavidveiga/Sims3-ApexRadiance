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
// Historical Steam 1.67 fingerprint, checked only inside the first bounded
// copy of the loaded .text section. A match is never treated as hook validation.
constexpr std::array<BYTE, 13> kPriorSteamResolver{
    0x81, 0xEC, 0x08, 0x08, 0x00, 0x00, 0x53,
    0x55, 0x8B, 0xAC, 0x24, 0x14, 0x08
};

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

    // Report page protection as observed by Windows, not as an ABI inference.
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(textAt), &page, sizeof(page)))
        return "Could not query live .text page protection";

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
            "No hooks installed (page protect={:#x}).", bits, sample, page.Protect);
    }

    // Inspect the bytes we already safely copied. Do not synchronously scan
    // megabytes of live, potentially guarded/protected code from an ImGui click.
    const auto match = std::search(bytes.begin(), bytes.end(),
                                   kPriorSteamResolver.begin(),
                                   kPriorSteamResolver.end());
    if (match != bytes.end()) {
        const auto offset = static_cast<std::size_t>(
            std::distance(bytes.begin(), match));
        return std::format(
            "Loaded .text sample entropy {:.3f}; historical Steam-like bytes "
            "at sample RVA {:#x}; page protect={:#x}. Candidate UNVERIFIED; no hooks installed.",
            bits, text.VirtualAddress + offset, page.Protect);
    }
    return std::format(
        "Loaded .text sample entropy {:.3f} ({} bytes); historical Steam-like "
        "prologue absent from sample; page protect={:#x}. Native Mono ABI unverified; no hooks installed.",
        bits, sample, page.Protect);
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
