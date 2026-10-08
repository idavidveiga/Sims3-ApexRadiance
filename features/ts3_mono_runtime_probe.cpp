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

constexpr std::size_t kScanStepBytes = 65536;
constexpr std::size_t kMaxReportedMatches = 8;

// UI-thread-only state: at most 64 KiB of the loaded executable is inspected
// each frame while this developer page is open. No background access, hooks,
// writes, or attempt to bypass the game's activation/DRM is made.
struct ExtendedScan {
    bool active = false;
    uintptr_t moduleBase = 0;
    uintptr_t textBase = 0;
    std::size_t length = 0;
    std::size_t next = 0;
    std::size_t readable = 0;
    std::size_t skipped = 0;
    std::size_t matches = 0;
    std::vector<uint32_t> candidateRvas;
    std::array<BYTE, kPriorSteamResolver.size() - 1> tail{};
    std::size_t tailLength = 0;
    std::string status;
};
ExtendedScan g_scan;

bool ReadLiveTextBounds(uintptr_t& base, uintptr_t& imageBase,
                        std::size_t& length, std::string& why) {
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!image) { why = "Main module unavailable"; return false; }
    IMAGE_DOS_HEADER dos{};
    if (!MemPatch::ReadBytes(image, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 0x4000) {
        why = "Loaded DOS header invalid"; return false;
    }
    const uintptr_t ntAt = image + static_cast<uint32_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS32 nt{};
    if (!MemPatch::ReadBytes(ntAt, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > 32 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) {
        why = "Loaded PE32 headers invalid"; return false;
    }
    const uintptr_t sectionAt =
        ntAt + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!MemPatch::ReadBytes(sectionAt + i * sizeof(section),
                                 &section, sizeof(section))) {
            why = "Could not read section headers"; return false;
        }
        if (std::memcmp(section.Name, ".text", 5) != 0) continue;
        if (!section.Misc.VirtualSize ||
            section.VirtualAddress >= nt.OptionalHeader.SizeOfImage) {
            why = "Loaded .text section has invalid bounds"; return false;
        }
        length = std::min<std::size_t>(
            section.Misc.VirtualSize,
            nt.OptionalHeader.SizeOfImage - section.VirtualAddress);
        if (length > 64u * 1024u * 1024u) {
            why = "Loaded .text section exceeds 64 MiB safety limit"; return false;
        }
        imageBase = image;
        base = image + section.VirtualAddress;
        return true;
    }
    why = "No loaded .text section"; return false;
}

bool IsReadable(const MEMORY_BASIC_INFORMATION& page) {
    if (page.State != MEM_COMMIT || (page.Protect & PAGE_GUARD) ||
        (page.Protect & PAGE_NOACCESS)) return false;
    switch (page.Protect & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

void StartExtendedScan() {
    g_scan = {};
    std::string problem;
    if (!ReadLiveTextBounds(g_scan.textBase, g_scan.moduleBase,
                            g_scan.length, problem)) {
        g_scan.status = problem;
        LOG_WARNING(std::format("[TS3 Mono Extended Probe] {}", problem));
        return;
    }
    g_scan.active = true;
    g_scan.status = "Scanning loaded .text in 64 KiB steps (read-only)";
    LOG_INFO(std::format("[TS3 Mono Extended Probe] Started: .text RVA {:#x}, {} bytes",
                         g_scan.textBase - g_scan.moduleBase, g_scan.length));
}

void FinishExtendedScan() {
    g_scan.active = false;
    std::string examples;
    for (const uint32_t rva : g_scan.candidateRvas)
        examples += std::format(" {:#x}", rva);
    g_scan.status = std::format(
        "Complete: {}/{} bytes read, {} skipped; historical Steam-shaped "
        "signature: {} match(es){}; ABI and JIT method UNVERIFIED",
        g_scan.readable, g_scan.length, g_scan.skipped,
        g_scan.matches, examples);
    LOG_INFO(std::format("[TS3 Mono Extended Probe] {}", g_scan.status));
}

void StepExtendedScan() {
    if (!g_scan.active) return;
    if (g_scan.next >= g_scan.length) { FinishExtendedScan(); return; }
    const uintptr_t address = g_scan.textBase + g_scan.next;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &page, sizeof(page)) ||
        !page.RegionSize) {
        g_scan.active = false;
        g_scan.status = "Stopped: VirtualQuery failed on loaded .text";
        LOG_WARNING(std::format("[TS3 Mono Extended Probe] {}", g_scan.status));
        return;
    }
    const auto pageStart = reinterpret_cast<uintptr_t>(page.BaseAddress);
    if (pageStart > address || page.RegionSize > UINTPTR_MAX - pageStart) {
        g_scan.active = false;
        g_scan.status = "Stopped: unexpected virtual memory region bounds";
        LOG_WARNING(std::format("[TS3 Mono Extended Probe] {}", g_scan.status));
        return;
    }
    const uintptr_t regionEnd = pageStart + page.RegionSize;
    const std::size_t remaining = g_scan.length - g_scan.next;
    const std::size_t take = std::min({
        remaining, kScanStepBytes, static_cast<std::size_t>(regionEnd - address)
    });
    if (!take) {
        g_scan.active = false;
        g_scan.status = "Stopped: zero-length virtual memory region";
        LOG_WARNING(std::format("[TS3 Mono Extended Probe] {}", g_scan.status));
        return;
    }
    if (!IsReadable(page)) {
        g_scan.skipped += take;
        g_scan.next += take;
        g_scan.tailLength = 0;
        return;
    }
    std::vector<BYTE> copied(g_scan.tailLength + take);
    std::copy_n(g_scan.tail.begin(), g_scan.tailLength, copied.begin());
    if (!MemPatch::ReadBytes(address, copied.data() + g_scan.tailLength, take)) {
        g_scan.skipped += take;
        g_scan.next += take;
        g_scan.tailLength = 0;
        return;
    }
    g_scan.readable += take;
    for (auto it = std::search(copied.begin(), copied.end(),
                               kPriorSteamResolver.begin(),
                               kPriorSteamResolver.end());
         it != copied.end();
         it = std::search(std::next(it), copied.end(),
                          kPriorSteamResolver.begin(),
                          kPriorSteamResolver.end())) {
        ++g_scan.matches;
        if (g_scan.candidateRvas.size() < kMaxReportedMatches) {
            const std::size_t at = g_scan.next - g_scan.tailLength +
                static_cast<std::size_t>(std::distance(copied.begin(), it));
            g_scan.candidateRvas.push_back(static_cast<uint32_t>(
                g_scan.textBase - g_scan.moduleBase + at));
        }
    }
    g_scan.tailLength = std::min(g_scan.tail.size(), copied.size());
    std::copy(copied.end() - g_scan.tailLength, copied.end(), g_scan.tail.begin());
    g_scan.next += take;
    if (g_scan.next >= g_scan.length) FinishExtendedScan();
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
    ImGui::Separator();
    if (ImGui::Button("Scan entire loaded .text (read-only, incremental)"))
        StartExtendedScan();
    // Exactly one bounded chunk per render frame, only while the developer
    // performance page is visible. A repeated click restarts the scan safely.
    StepExtendedScan();
    if (g_scan.active && g_scan.length) {
        ImGui::ProgressBar(static_cast<float>(g_scan.next) /
                               static_cast<float>(g_scan.length),
                           ImVec2(-1.0f, 0.0f));
    }
    if (!g_scan.status.empty())
        ImGui::TextWrapped("%s", g_scan.status.c_str());
    ImGui::TextDisabled("Signatures are observations only; no native JIT method or ABI is validated.");
}
} // namespace Ts3MonoRuntimeProbe
