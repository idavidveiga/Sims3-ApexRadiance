// TS3 EA 1.69 ships an activation-protected on-disk PE whose .text entropy is
// effectively 8 bits/byte. An unverified disk signature is NOT an ABI proof.
// This is a manual, read-only in-process diagnostic run after normal game startup.
// No hooks, no writes, no method swapping, no external modding libraries.
#include "ts3_mono_runtime_probe.h"
#include "ts3_mono_xref.h"
#include "ts3_cas_mono_sites.h"
#include "ts3_mono_host_snapshot.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "game_version.h"
#include "game_addresses.h"
#include "entry_chain.h"
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
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ts3MonoRuntimeProbe {
namespace {
std::string g_status =
    "Not inspected. Run this only after the world has finished loading.";
std::string g_liveLogStatus;
std::string g_monoAnchorsStatus = "Mono runtime anchors not inspected.";
constexpr size_t kSampleBytes = 65536;
// Evidence correction (2026-10-08): public independent TS3 Mono 1.2.3.1
// research identifies these EXACT bytes on Steam as mono_lookup_internal_call.
// The user's EA 1.69 xrefs all pass one MonoMethod-like argument and cache
// its EAX result in the +0x20 union, where ICALL native pointers can live.
// These facts support an INTERNAL-CALL RESOLVER, not a JIT compiler or
// method-header helper. They do not validate EA native hook safety.
// The probe remains read-only; NEVER treat a byte match as hook approval.
constexpr std::array<BYTE, 13> kHistoricalMonoIcallCandidate{
    0x81, 0xEC, 0x08, 0x08, 0x00, 0x00, 0x53,
    0x55, 0x8B, 0xAC, 0x24, 0x14, 0x08
};

// Query only the already loaded modules' Windows export directories.
// This is much stronger than a guessed signature *if* a relevant Mono API
// is exported, but a name by itself is not a validated calling convention.
// GetProcAddress is used strictly as a lookup: none of these addresses are
// invoked, patched, or cached for use by game-facing features.
std::string InspectMonoExports() {
    struct Module {
        const char* label;
        HMODULE handle;
    };
    const std::array<Module, 4> modules{{
        {"TS3 main executable", GetModuleHandleW(nullptr)},
        {"mono.dll", GetModuleHandleW(L"mono.dll")},
        {"mono-2.0-bdwgc.dll", GetModuleHandleW(L"mono-2.0-bdwgc.dll")},
        {"mono-2.0-sgen.dll", GetModuleHandleW(L"mono-2.0-sgen.dll")}
    }};
    constexpr std::array<const char*, 7> names{{
        "mono_compile_method",
        "mono_jit_info_table_find",
        "mono_jit_info_get_method",
        "mono_method_get_token",
        "mono_method_get_name",
        "mono_method_desc_search_in_image",
        "mono_domain_get"
    }};
    unsigned found = 0;
    unsigned loaded = 0;
    for (const Module& m : modules) {
        if (!m.handle) continue;
        ++loaded;
        for (const char* name : names) {
            const FARPROC symbol = GetProcAddress(m.handle, name);
            if (!symbol) continue;
            ++found;
            LOG_INFO(std::format(
                "[TS3 Mono Export Probe] {} exports {} at {:#010x} (lookup only)",
                m.label, name, reinterpret_cast<uintptr_t>(symbol)));
        }
    }
    const std::string status = std::format(
        "{} loaded module(s), {} matching Mono export(s). "
        "No exports does not mean Mono is absent: the runtime may be "
        "statically embedded. No function was called or hooked.",
        loaded, found);
    LOG_INFO(std::format("[TS3 Mono Export Probe] {}", status));
    return status;
}

// Focused, non-invasive baseline for later Mono JIT identification.
// These signatures come from independent existing ScriptTypeCache research.
// A known Mono runtime function is an anchor, NOT a JIT compiler or an
// authorization to install a hook. Never invoke the address or change pages.
std::string InspectMonoRuntimeAnchors() {
    if (!GameAddr::Scanned())
        return "GameAddress initialization not finished. Open this page after world load.";
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return "Main TS3 executable is unavailable";
    struct Anchor {
        GameAddr::Id id;
        EntryChain::Site site;
        std::array<BYTE, 8> expected;
        std::size_t length;
    };
    constexpr Anchor anchors[] = {
        {GameAddr::Id::MonoTypeGetObject, EntryChain::Site::MonoTypeGetObject,
         {0x53,0x8B,0x5C,0x24,0x0C,0x55,0x56,0x57}, 8},
        {GameAddr::Id::MonoDomainFree, EntryChain::Site::MonoDomainFree,
         {0x55,0x56,0x8B,0x74,0x24,0x0C,0x00,0x00}, 6}
    };
    std::string status;
    unsigned accepted = 0;
    for (const Anchor& a : anchors) {
        const uintptr_t found = GameAddr::Get(a.id);
        std::array<BYTE, 8> bytes{};
        const bool readable = found && found >= base &&
            MemPatch::ReadBytes(found, bytes.data(), a.length);
        const bool original = readable &&
            std::memcmp(bytes.data(), a.expected.data(), a.length) == 0;

        // An active ScriptMath EntryChain layer replaces the first five
        // original bytes with an Apex-owned JMP. Inspect its saved game
        // prologue in the trampoline, but only if the live JMP STILL points
        // to the Apex-registered hook. Otherwise report a potential conflict.
        const bool scriptMathLayer = EntryChain::Installed(
            a.site, EntryChain::Layer::ScriptMath);
        const bool ownEntry = scriptMathLayer &&
            EntryChain::GameFunction(a.site) == found &&
            EntryChain::OwnsEntry(a.site);
        const uintptr_t trampoline = reinterpret_cast<uintptr_t>(
            EntryChain::Original(a.site));
        std::array<BYTE, 8> saved{};
        const bool savedOriginal = ownEntry && trampoline &&
            MemPatch::ReadBytes(trampoline, saved.data(), a.length) &&
            std::memcmp(saved.data(), a.expected.data(), a.length) == 0;

        // A registered hook plus a pristine entry is not a valid coherent
        // state; a matching unpatched prologue counts only with no layer.
        const bool acceptedEntry = (!scriptMathLayer && original) ||
                                   (scriptMathLayer && ownEntry && savedOriginal);
        const char* state = !found ? "not resolved" :
                            !readable ? "unreadable" :
                            scriptMathLayer && !ownEntry
                                ? "Apex hook registered, but entry ownership lost (possible conflict)" :
                            scriptMathLayer && !savedOriginal
                                ? "Apex hook active; original trampoline prologue mismatch" :
                            scriptMathLayer
                                ? "Apex ScriptMath hook active; original prologue verified in trampoline" :
                            original ? "known original prologue matches" :
                                       "modified from known prologue (unknown owner)";
        const std::string entry = std::format(
            "{}: {}{}",
            GameAddr::Name(a.id), state,
            found && found >= base
                ? std::format(" (RVA {:#x})", found - base)
                : std::string{});
        LOG_INFO(std::format("[TS3 Mono Anchors] {}", entry));
        if (!status.empty()) status += " | ";
        status += entry;
        if (acceptedEntry) ++accepted;
    }
    LOG_INFO(std::format(
        "[TS3 Mono Anchors] {}/2 known Mono entry points validated "
        "(including intact Apex-owned trampolines); does NOT identify "
        "the CAS method or authorize a new hook", accepted));
    return status + ". Read-only checks; no managed method inferred.";
}

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
                                   kHistoricalMonoIcallCandidate.begin(),
                                   kHistoricalMonoIcallCandidate.end());
    if (match != bytes.end()) {
        const auto offset = static_cast<std::size_t>(
            std::distance(bytes.begin(), match));
        return std::format(
            "Loaded .text sample entropy {:.3f}; historical Steam-like bytes "
            "at sample RVA {:#x}; page protect={:#x}. Resolver candidate UNVERIFIED on EA; no hooks installed.",
            bits, text.VirtualAddress + offset, page.Protect);
    }
    return std::format(
        "Loaded .text sample entropy {:.3f} ({} bytes); historical Steam-like "
        "prologue absent from sample; page protect={:#x}. Native Mono ABI unverified; no hooks installed.",
        bits, sample, page.Protect);
}

constexpr std::size_t kScanStepBytes = 65536;
constexpr std::size_t kMaxReportedMatches = 8;
// Bounded candidate code sample for independent offline x86 instruction review.
// Extracted from the existing 64-KiB read buffer: no additional process reads.
constexpr std::size_t kCandidateBytesAfter = 512;

// UI-thread-only state: at most 64 KiB of the loaded executable is inspected
// each frame while this developer page is open. No background access, hooks,
// writes, or attempt to bypass the game's activation/DRM is made.
struct ExtendedScan {
    bool active = false;
    uintptr_t moduleBase = 0;
    uintptr_t textBase = 0;
    std::uint32_t imageSize = 0;
    std::size_t length = 0;
    std::size_t next = 0;
    std::size_t readable = 0;
    std::size_t skipped = 0;
    std::size_t matches = 0;
    std::vector<uint32_t> candidateRvas;
    std::vector<std::string> candidateWindows; // bounded nearby byte context, read from existing copy
    ApexCasMono::BridgeSignatureEvidence bridgeEvidence{};
    // Tail covers the longest bridge signature. Discard fully prefixed
    // ICall occurrences so they are not counted twice.
    std::array<BYTE, ApexCasMono::kMaxBridgePatternLength - 1> tail{};
    std::size_t tailLength = 0;
    std::string status;
};
ExtendedScan g_scan;

// A second *read-only* pass checks byte-level x86 CALL rel32 references
// to candidates found during the historical-signature scan. CALL bytes
// inside data can be false positives; no signature or reference proves
// a Mono JIT entry point, native calling convention or viable hook.
struct ReferenceScan {
    bool active = false;
    std::size_t next = 0;
    std::size_t readable = 0;
    std::size_t skipped = 0;
    std::array<BYTE, 4> tail{};
    std::size_t tailLength = 0;
    std::vector<std::size_t> counts;
    std::vector<std::string> samples;
    std::string status;
};
ReferenceScan g_refs;
constexpr std::size_t kMaxReferenceSamples = 24;

bool ReadLiveTextBounds(uintptr_t& base, uintptr_t& imageBase,
                        std::size_t& length, std::uint32_t& imageSize,
                        std::string& why) {
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
        imageSize = nt.OptionalHeader.SizeOfImage;
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

void StartReferenceScan() {
    g_refs = {};
    if (g_scan.candidateRvas.empty()) {
        g_refs.status = "No candidates: direct-call reference pass not applicable.";
        return;
    }
    // Do not make claims about an arbitrary subset when there were more
    // historical-signature matches than our bounded candidate capacity.
    if (g_scan.matches != g_scan.candidateRvas.size()) {
        g_refs.status =
            "Too many ambiguous signature matches; reference pass skipped.";
        LOG_WARNING(std::format("[TS3 Mono Reference Probe] {}", g_refs.status));
        return;
    }
    g_refs.counts.assign(g_scan.candidateRvas.size(), 0);
    g_refs.active = true;
    g_refs.status = "Inspecting loaded .text for raw E8 CALL references (read-only)";
    LOG_INFO(std::format(
        "[TS3 Mono Reference Probe] Started read-only CALL reference pass for {} candidate(s)",
        g_scan.candidateRvas.size()));
}

void FinishReferenceScan() {
    g_refs.active = false;
    g_refs.status = std::format(
        "Complete: {}/{} bytes checked, {} skipped. Raw CALL-byte references "
        "are only clues, NOT method or ABI proof.",
        g_refs.readable, g_scan.length, g_refs.skipped);
    LOG_INFO(std::format("[TS3 Mono Reference Probe] {}", g_refs.status));
    for (std::size_t i = 0; i < g_refs.counts.size(); ++i) {
        LOG_INFO(std::format(
            "[TS3 Mono Reference Probe] Candidate RVA {:#x}: {} raw E8 rel32 references",
            g_scan.candidateRvas[i], g_refs.counts[i]));
    }
    for (const auto& sample : g_refs.samples)
        LOG_INFO(std::format("[TS3 Mono Reference Probe] {}", sample));
}

void StepReferenceScan() {
    if (!g_refs.active) return;
    if (g_refs.next >= g_scan.length) { FinishReferenceScan(); return; }

    const uintptr_t address = g_scan.textBase + g_refs.next;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &page, sizeof(page)) ||
        !page.RegionSize) {
        g_refs.active = false;
        g_refs.status = "Stopped: VirtualQuery failed during reference scan";
        LOG_WARNING(std::format("[TS3 Mono Reference Probe] {}", g_refs.status));
        return;
    }
    const uintptr_t pageStart = reinterpret_cast<uintptr_t>(page.BaseAddress);
    if (pageStart > address || page.RegionSize > UINTPTR_MAX - pageStart) {
        g_refs.active = false;
        g_refs.status = "Stopped: unexpected reference scan page bounds";
        LOG_WARNING(std::format("[TS3 Mono Reference Probe] {}", g_refs.status));
        return;
    }
    const uintptr_t end = pageStart + page.RegionSize;
    const std::size_t take = std::min({
        g_scan.length - g_refs.next, kScanStepBytes,
        static_cast<std::size_t>(end - address)
    });
    if (!take) {
        g_refs.active = false;
        g_refs.status = "Stopped: zero-length reference scan page";
        LOG_WARNING(std::format("[TS3 Mono Reference Probe] {}", g_refs.status));
        return;
    }
    if (!IsReadable(page)) {
        g_refs.skipped += take;
        g_refs.next += take;
        g_refs.tailLength = 0;
        return;
    }

    std::vector<BYTE> copied(g_refs.tailLength + take);
    std::copy_n(g_refs.tail.begin(), g_refs.tailLength, copied.begin());
    if (!MemPatch::ReadBytes(address, copied.data() + g_refs.tailLength, take)) {
        g_refs.skipped += take;
        g_refs.next += take;
        g_refs.tailLength = 0;
        return;
    }
    g_refs.readable += take;
    // Cross-chunk E8 opcode + rel32 may straddle a 64-KiB boundary.
    // Carry only the previous four contiguous readable bytes.
    const uint32_t blockRva = static_cast<uint32_t>(
        g_scan.textBase - g_scan.moduleBase +
        g_refs.next - g_refs.tailLength);
    Ts3MonoXref::VisitDirectCallsToCandidates(
        std::span<const BYTE>(copied.data(), copied.size()), blockRva,
        std::span<const uint32_t>(g_scan.candidateRvas.data(),
                                  g_scan.candidateRvas.size()),
        [&](Ts3MonoXref::Reference ref) {
            const auto it = std::find(g_scan.candidateRvas.begin(),
                                      g_scan.candidateRvas.end(),
                                      ref.candidateRva);
            if (it == g_scan.candidateRvas.end()) return;
            const std::size_t index = static_cast<std::size_t>(
                std::distance(g_scan.candidateRvas.begin(), it));
            ++g_refs.counts[index];
            if (g_refs.samples.size() < kMaxReferenceSamples) {
                // Capture a bounded neighborhood from the buffer that
                // was ALREADY copied for this scan. No extra process read.
                // Bytes preceding a chunk edge can be shorter than requested;
                // the opcode location is always explicitly marked.
                constexpr std::size_t kBefore = 24;
                constexpr std::size_t kAfter = 96;
                const std::size_t index = static_cast<std::size_t>(
                    ref.callerRva - blockRva);
                const std::size_t begin = index > kBefore ? index - kBefore : 0;
                const std::size_t end = std::min(copied.size(),
                    index + std::size_t{5} + kAfter);
                std::string sample = std::format(
                    "Caller RVA {:#x} -> target RVA {:#x}; -{} / +{} bytes; "
                    "raw E8 offset marked by | : ",
                    ref.callerRva, ref.candidateRva,
                    index - begin, end - (index + 5));
                for (std::size_t j = begin; j < end; ++j) {
                    if (j == index || j == index + 5)
                        sample += " |";
                    sample += std::format(" {:02X}",
                        static_cast<unsigned>(copied[j]));
                }
                sample += " [instruction boundary and JIT identity unverified]";
                g_refs.samples.push_back(std::move(sample));
            }
        });
    g_refs.tailLength = std::min(g_refs.tail.size(), copied.size());
    std::copy(copied.end() - g_refs.tailLength, copied.end(), g_refs.tail.begin());
    g_refs.next += take;
    if (g_refs.next >= g_scan.length) FinishReferenceScan();
}

void StartExtendedScan() {
    g_scan = {};
    g_refs = {};
    std::string problem;
    if (!ReadLiveTextBounds(g_scan.textBase, g_scan.moduleBase,
                            g_scan.length, g_scan.imageSize, problem)) {
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
        "Complete: {}/{} bytes read, {} skipped; historical ICall resolver "
        "signature: {} match(es){}; resolver ABI UNVERIFIED; no JIT inference",
        g_scan.readable, g_scan.length, g_scan.skipped,
        g_scan.matches, examples);
    LOG_INFO(std::format("[TS3 Mono Extended Probe] {}", g_scan.status));
    for (const auto& context : g_scan.candidateWindows)
        LOG_INFO(std::format("[TS3 Mono Extended Probe] Candidate bytes: {}", context));
    unsigned unique = 0;
    for (const auto& p : ApexCasMono::kBridgePatterns) {
        const auto& m = g_scan.bridgeEvidence.Get(p.site);
        if (m.Unique()) ++unique;
        LOG_INFO(std::format(
            "[TS3 CAS Mono Bridge] {}: {} independent entry pattern match(es){} "
            "(read-only, ABI NOT verified)",
            p.name, m.count,
            m.Unique() ? std::format(", candidate RVA {:#x}", m.firstRva) : std::string{}));
    }
    LOG_INFO(std::format(
        "[TS3 CAS Mono Bridge] {}/{} unique structural entries. "
        "No native function was invoked and NO Hair/Hats hook is installed.",
        unique, ApexCasMono::kSiteCount));

    // Optional READ-ONLY snapshot: derive the ScriptHost global from the
    // exact InitHeap CMP instruction, then read its host/domain pointers.
    // No Mono API is called; no pointer is retained after this function.
    const auto& initHeap = g_scan.bridgeEvidence.Get(
        ApexCasMono::BridgeSite::ScriptHostInitHeap);
    if (initHeap.Unique() && g_scan.imageSize &&
        g_scan.moduleBase <= UINT32_MAX &&
        initHeap.firstRva < g_scan.imageSize &&
        g_scan.moduleBase + initHeap.firstRva <= UINT32_MAX) {
        const auto snapshot = ApexCasMono::InspectMonoHost(
            static_cast<std::uint32_t>(g_scan.moduleBase),
            g_scan.imageSize,
            static_cast<std::uint32_t>(g_scan.moduleBase + initHeap.firstRva),
            [](std::uint32_t address, void* destination, std::size_t length) {
                return MemPatch::ReadBytes(address, destination, length);
            });
        LOG_INFO(std::format(
            "[TS3 CAS Mono Bridge] ScriptHost snapshot: {}{}; "
            "does NOT verify Mono function ABIs or safe managed continuation",
            ApexCasMono::HostStatusName(snapshot.status),
            snapshot.globalRva
                ? std::format(" (global RVA {:#x})", snapshot.globalRva)
                : std::string{}));
    } else {
        LOG_INFO(
            "[TS3 CAS Mono Bridge] ScriptHost snapshot unavailable: "
            "InitHeap entry absent/ambiguous, or executable bounds invalid.");
    }
    // Automatically follow a completed signature pass with a read-only
    // reference pass; no extra user click and no additional hooks needed.
    StartReferenceScan();
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
    const std::size_t scannedOffset = g_scan.next - g_scan.tailLength;
    // Reuse the same bounded, read-only memory block for Mono bridge sites.
    g_scan.bridgeEvidence.Observe(
        std::span<const std::uint8_t>(copied.data(), copied.size()),
        static_cast<std::uint32_t>(g_scan.textBase - g_scan.moduleBase + scannedOffset),
        g_scan.tailLength);
    for (auto it = std::search(copied.begin(), copied.end(),
                               kHistoricalMonoIcallCandidate.begin(),
                               kHistoricalMonoIcallCandidate.end());
         it != copied.end();
         it = std::search(std::next(it), copied.end(),
                          kHistoricalMonoIcallCandidate.begin(),
                          kHistoricalMonoIcallCandidate.end())) {
        const std::size_t indexFound =
            static_cast<std::size_t>(std::distance(copied.begin(), it));
        if (indexFound + kHistoricalMonoIcallCandidate.size() <=
            g_scan.tailLength) continue;
        ++g_scan.matches;
        if (g_scan.candidateRvas.size() < kMaxReportedMatches) {
            const std::size_t at = g_scan.next - g_scan.tailLength +
                static_cast<std::size_t>(std::distance(copied.begin(), it));
            const auto rva = static_cast<uint32_t>(
                g_scan.textBase - g_scan.moduleBase + at);
            g_scan.candidateRvas.push_back(rva);
            // Record at most eight small neighborhoods around exact matches.
            // This consumes bytes already copied from the user's own loaded game;
            // it never reads additional memory, installs hooks or patches any code.
            const std::size_t index =
                static_cast<std::size_t>(std::distance(copied.begin(), it));
            const std::size_t start = index > 16 ? index - 16 : 0;
            const std::size_t end = std::min(copied.size(),
                index + kHistoricalMonoIcallCandidate.size() + kCandidateBytesAfter);
            std::string context = std::format("RVA {:#x}; -{} / +{} bytes: ",
                rva, index - start, end - index);
            for (std::size_t j = start; j < end; ++j) {
                if (j == index || j == index + kHistoricalMonoIcallCandidate.size())
                    context += " |";
                context += std::format(" {:02X}", static_cast<unsigned>(copied[j]));
            }
            g_scan.candidateWindows.push_back(std::move(context));
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
    if (ImGui::Button("Inspect known Mono runtime anchors (read-only)")) {
        g_monoAnchorsStatus = InspectMonoRuntimeAnchors();
        g_monoAnchorsStatus += " " + InspectMonoExports();
    }
    ImGui::TextWrapped("%s", g_monoAnchorsStatus.c_str());
    ImGui::TextDisabled(
        "These known functions are only starting anchors; never use them as a JIT hook.");
    ImGui::Separator();
    if (ImGui::Button("Scan entire loaded .text (read-only, incremental)"))
        StartExtendedScan();
    // Exactly one bounded chunk per render frame, only while the developer
    // performance page is visible. A repeated click restarts the scan safely.
    // Each frame reads at most one 64-KiB chunk, including on the frame
    // when the signature scan completes and activates the reference pass.
    const bool signatureWasActive = g_scan.active;
    StepExtendedScan();
    if (!signatureWasActive) StepReferenceScan();
    if (g_scan.active && g_scan.length) {
        ImGui::ProgressBar(static_cast<float>(g_scan.next) /
                               static_cast<float>(g_scan.length),
                           ImVec2(-1.0f, 0.0f));
    }
    if (!g_scan.status.empty())
        ImGui::TextWrapped("%s", g_scan.status.c_str());
    if (g_refs.active && g_scan.length) {
        ImGui::ProgressBar(static_cast<float>(g_refs.next) /
                               static_cast<float>(g_scan.length),
                           ImVec2(-1.0f, 0.0f));
    }
    if (!g_refs.status.empty()) {
        ImGui::TextWrapped("%s", g_refs.status.c_str());
        for (std::size_t i = 0; i < g_refs.counts.size(); ++i) {
            ImGui::TextDisabled("Candidate RVA %#x: %zu raw E8 references",
                g_scan.candidateRvas[i], g_refs.counts[i]);
        }
        ImGui::TextDisabled("Raw byte matches do NOT verify ICall ABI or managed-method identity.");
    }
    ImGui::Separator();
    if (ImGui::Button("Save live log copy (keep game open)")) {
        if (ApexPaths::EnsureApexDirectory() &&
            ApexLog::SaveSnapshot(ApexPaths::ApexDirectory() +
                                  L"ApexRadiance_LOG_LIVE.txt")) {
            g_liveLogStatus =
                "Saved ApexRadiance_LOG_LIVE.txt to your Apex Radiance folder. "
                "Upload this copy; you may keep the game running.";
        } else {
            g_liveLogStatus =
                "Could not export the live log. Close the game to collect the "
                "normal log, or check folder permissions.";
        }
    }
    if (!g_liveLogStatus.empty())
        ImGui::TextWrapped("%s", g_liveLogStatus.c_str());
    ImGui::TextWrapped(
        "EA 1.69 research result: RVA 0xA826A0 has four one-argument "
        "callers caching EAX at MonoMethod-like object+0x20. Independent "
        "TS3 Mono research identifies these bytes as mono_lookup_internal_call "
        "(native ICall resolver), NOT a JIT compiler. Identity/ABI must "
        "still be independently verified before any hook.");
    ImGui::TextDisabled(
        "Read-only diagnostics; interpreter method identity and ABI remain unverified.");
}
} // namespace Ts3MonoRuntimeProbe
