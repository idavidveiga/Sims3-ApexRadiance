#include "lot_active_threshold.h"
#include "apex_log.h"
#include "memory_patch.h"
#include "game_version.h"
#include "s3ss_detect.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kSettingName[] = L"Throttle Lot LoD Transitions Max Active Lot Threshold";
constexpr int kPlausibleMin = 0;
constexpr int kPlausibleMax = 128;

struct SectionRange {
    BYTE* begin = nullptr;
    size_t size = 0;
    DWORD characteristics = 0;
    char name[9]{};
};

std::mutex g_lock;
bool g_running = false;
bool g_externalOwner = false;
uintptr_t g_address = 0;
bool g_originalValid = false;
bool g_written = false;
bool g_maintainLogged = false;
bool g_badValueLogged = false;
int g_original = 0;

bool S3SSOwnsStreamingSettings() {
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "streamingSettings", true);
}

bool ReadInt(uintptr_t address, int& value) {
    return address && MemPatch::ReadBytes(address, &value, sizeof(value));
}

bool Plausible(int value) { return value >= kPlausibleMin && value <= kPlausibleMax; }

bool WriteExpectedInt(uintptr_t address, int desired, int expected) {
    const DWORD d = static_cast<DWORD>(desired);
    const DWORD e = static_cast<DWORD>(expected);
    return MemPatch::WriteDWORD(address, d, nullptr, &e);
}

bool GetMainSections(std::vector<SectionRange>& out) {
    out.clear();
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;

    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const size_t size = std::max<size_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        if (!size) continue;
        SectionRange r;
        r.begin = base + sec[i].VirtualAddress;
        r.size = size;
        r.characteristics = sec[i].Characteristics;
        std::memcpy(r.name, sec[i].Name, 8);
        r.name[8] = 0;
        out.push_back(r);
    }
    return !out.empty();
}

bool AddressInWritableSection(uintptr_t address, const std::vector<SectionRange>& sections) {
    if (!address) return false;
    for (const auto& s : sections) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(s.begin);
        const uintptr_t hi = lo + s.size;
        if ((s.characteristics & IMAGE_SCN_MEM_WRITE) && address >= lo && address + sizeof(int) <= hi) return true;
    }
    return false;
}

void AddUnique(std::vector<uintptr_t>& values, uintptr_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

bool HasPushRegister(const BYTE* p, size_t available, int reg) {
    const BYTE opcode = static_cast<BYTE>(0x50 + (reg & 7));
    const size_t n = std::min<size_t>(available, 10);
    for (size_t i = 0; i < n; ++i)
        if (p[i] == opcode) return true;
    return false;
}

void ConsiderCandidate(uintptr_t candidate, const std::vector<SectionRange>& sections, std::vector<uintptr_t>& candidates) {
    if (!AddressInWritableSection(candidate, sections) || (candidate & 3u) != 0) return;
    int value = 0;
    if (!ReadInt(candidate, value) || !Plausible(value)) return;
    AddUnique(candidates, candidate);
}

uintptr_t ResolveLiveSettingAddress(std::string* error) {
    std::vector<SectionRange> sections;
    if (!GetMainSections(sections)) {
        if (error) *error = "Could not inspect the TS3 executable sections";
        return 0;
    }

    const BYTE* literalBytes = reinterpret_cast<const BYTE*>(kSettingName);
    const size_t literalSize = sizeof(kSettingName); // includes UTF-16 NUL
    std::vector<uintptr_t> literals;

    for (const auto& s : sections) {
        if ((s.characteristics & IMAGE_SCN_MEM_EXECUTE) || !(s.characteristics & IMAGE_SCN_MEM_READ) || s.size < literalSize) continue;
        for (size_t off = 0; off + literalSize <= s.size; ++off) {
            if (std::memcmp(s.begin + off, literalBytes, literalSize) == 0)
                AddUnique(literals, reinterpret_cast<uintptr_t>(s.begin + off));
        }
    }

    if (literals.empty()) {
        if (error) *error = "The live-setting name was not found in TS3.exe";
        return 0;
    }

    std::vector<uintptr_t> refs;
    std::vector<uintptr_t> candidates;

    for (const auto& s : sections) {
        if (!(s.characteristics & IMAGE_SCN_MEM_EXECUTE) || !(s.characteristics & IMAGE_SCN_MEM_READ) || s.size < 16) continue;

        for (uintptr_t literal : literals) {
            const uint32_t literal32 = static_cast<uint32_t>(literal);
            for (size_t off = 0; off + 6 <= s.size; ++off) {
                BYTE* p = s.begin + off;
                if (p[0] != 0x68 || std::memcmp(p + 1, &literal32, sizeof(literal32)) != 0) continue; // PUSH setting-name
                const uintptr_t ref = reinterpret_cast<uintptr_t>(p);
                AddUnique(refs, ref);

                // VariableRegistry's arguments are pushed right-to-left. The value pointer follows the setting-name
                // push in instruction order. Accept only module-writable, aligned pointers whose current int is sane.
                const size_t remaining = s.size - off;
                const size_t limit = std::min<size_t>(remaining, 40);
                for (size_t j = 5; j + 5 <= limit; ++j) {
                    const BYTE op = p[j];

                    if (op == 0x68) { // PUSH imm32
                        uint32_t imm = 0;
                        std::memcpy(&imm, p + j + 1, sizeof(imm));
                        ConsiderCandidate(static_cast<uintptr_t>(imm), sections, candidates);
                        if (!candidates.empty()) break; // ptr is the first plausible writable immediate after name
                    }

                    if (op >= 0xB8 && op <= 0xBF) { // MOV reg, imm32 ... PUSH reg
                        uint32_t imm = 0;
                        std::memcpy(&imm, p + j + 1, sizeof(imm));
                        const int reg = op - 0xB8;
                        if (HasPushRegister(p + j + 5, limit - (j + 5), reg)) {
                            const size_t before = candidates.size();
                            ConsiderCandidate(static_cast<uintptr_t>(imm), sections, candidates);
                            if (candidates.size() != before) break;
                        }
                    }

                    if (op == 0x8D && j + 6 <= limit) { // LEA reg,[disp32] ... PUSH reg
                        const BYTE modrm = p[j + 1];
                        if ((modrm & 0xC7) == 0x05) {
                            uint32_t disp = 0;
                            std::memcpy(&disp, p + j + 2, sizeof(disp));
                            const int reg = (modrm >> 3) & 7;
                            if (HasPushRegister(p + j + 6, limit - (j + 6), reg)) {
                                const size_t before = candidates.size();
                                ConsiderCandidate(static_cast<uintptr_t>(disp), sections, candidates);
                                if (candidates.size() != before) break;
                            }
                        }
                    }
                }
            }
        }
    }

    LOG_INFO(std::format("[LotActiveThreshold] Resolver: {} setting-name literal(s), {} registration reference(s), {} plausible value address(es)",
                         literals.size(), refs.size(), candidates.size()));

    if (candidates.size() != 1) {
        if (error) {
            if (candidates.empty())
                *error = "Could not prove the Max Active Lot Threshold value address from its live-setting registration";
            else
                *error = std::format("The live-setting registration produced {} plausible value addresses; refusing to guess", candidates.size());
        }
        return 0;
    }

    return candidates.front();
}

void ResetState() {
    g_address = 0;
    g_originalValid = false;
    g_written = false;
    g_maintainLogged = false;
    g_badValueLogged = false;
    g_original = 0;
}

} // namespace

namespace LotActiveThreshold {

bool Start(std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running) return true;

    LOG_INFO(std::format("[LotActiveThreshold] Starting LoD active-lot threshold on {}", GetGameVersionName()));

    if (S3SSOwnsStreamingSettings()) {
        g_externalOwner = true;
        g_running = true;
        LOG_INFO("[LotActiveThreshold] Official Sims3SettingsSetter owns LotStreamingOptimizations.streamingSettings; Apex makes no threshold writes");
        return true;
    }

    ResetState();
    g_address = ResolveLiveSettingAddress(error);
    if (!g_address) return false;

    int current = 0;
    if (!ReadInt(g_address, current) || !Plausible(current)) {
        if (error) *error = "Resolved Max Active Lot Threshold address is unreadable or implausible";
        ResetState();
        return false;
    }

    g_original = current;
    g_originalValid = true;
    LOG_INFO(std::format("[LotActiveThreshold] 'Throttle Lot LoD Transitions Max Active Lot Threshold' at {:#010x}: current {}", g_address, current));

    if (current != kDesiredThreshold) {
        if (!WriteExpectedInt(g_address, kDesiredThreshold, current)) {
            if (error) *error = "Could not apply LoD active-lot threshold 12";
            ResetState();
            return false;
        }
        g_written = true;
        LOG_INFO(std::format("[LotActiveThreshold] Max Active Lot Threshold: {} -> {}", current, kDesiredThreshold));
    } else {
        LOG_INFO("[LotActiveThreshold] Max Active Lot Threshold already equals 12; no initial write needed");
    }

    g_externalOwner = false;
    g_running = true;
    LOG_INFO("[LotActiveThreshold] LoD active-lot threshold active");
    return true;
}

void Tick() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running || g_externalOwner || !g_address) return;

    int current = 0;
    if (!ReadInt(g_address, current) || !Plausible(current)) {
        if (!g_badValueLogged) {
            LOG_WARNING("[LotActiveThreshold] Threshold maintenance skipped: current value is unreadable or implausible");
            g_badValueLogged = true;
        }
        return;
    }
    g_badValueLogged = false;

    if (current == kDesiredThreshold) return;

    if (WriteExpectedInt(g_address, kDesiredThreshold, current)) {
        g_written = true;
        if (!g_maintainLogged) {
            LOG_INFO(std::format("[LotActiveThreshold] Max Active Lot Threshold drifted {} -> {}; maintained at {}",
                                 current, kDesiredThreshold, kDesiredThreshold));
            g_maintainLogged = true;
        }
    } else {
        LOG_WARNING("[LotActiveThreshold] Could not maintain Max Active Lot Threshold at 12");
    }
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    if (g_externalOwner) {
        LOG_INFO("[LotActiveThreshold] Off in Apex; Sims3SettingsSetter remains the owner");
    } else if (g_written && g_originalValid && g_address) {
        int current = 0;
        if (!ReadInt(g_address, current)) {
            LOG_WARNING("[LotActiveThreshold] Restore skipped: current value is unreadable");
        } else if (current != kDesiredThreshold) {
            LOG_INFO(std::format("[LotActiveThreshold] Restore skipped: another owner changed 12 to {}", current));
        } else if (WriteExpectedInt(g_address, g_original, current)) {
            LOG_INFO(std::format("[LotActiveThreshold] Max Active Lot Threshold restored to {}", g_original));
        } else {
            LOG_WARNING("[LotActiveThreshold] Restore failed; current value was left unchanged");
        }
    }

    LOG_INFO("[LotActiveThreshold] LoD active-lot threshold stopped");
    g_running = false;
    g_externalOwner = false;
    ResetState();
}

bool Running() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running;
}

bool HandledByS3SS() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalOwner;
}

std::string StatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return "Off";
    if (g_externalOwner) return "Handled by Sims3SettingsSetter";
    if (!g_address) return "On; waiting for the live setting";
    return std::format("On; internal LoD active-lot threshold 12 at {:#010x}", g_address);
}

} // namespace LotActiveThreshold
