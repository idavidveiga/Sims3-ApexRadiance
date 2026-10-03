#include "lot_visibility_override.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include "s3ss_detect.h"
#include <cstdint>
#include <format>
#include <mutex>

namespace {

std::mutex g_lock;
bool g_running = false;
bool g_externalS3SS = false;
bool g_externalPatched = false;
bool g_owned = false;
uintptr_t g_address = 0;

bool S3SSOwnsVisibilityOverride() {
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "visibilityOverride", true);
}

bool ReadByte(uintptr_t address, uint8_t& value) {
    return address && MemPatch::ReadBytes(address, &value, sizeof(value));
}

bool WriteExpectedByte(uintptr_t address, uint8_t desired, uint8_t expected) {
    const std::vector<BYTE> bytes{desired};
    const std::vector<BYTE> expectedBytes{expected};
    return MemPatch::WriteBytes(address, bytes, nullptr, &expectedBytes);
}

void ResetState() {
    g_address = 0;
    g_owned = false;
    g_externalPatched = false;
}

} // namespace

namespace LotVisibilityOverride {

bool Start(std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running) return true;

    LOG_INFO(std::format("[LotVisibility] Starting camera-bias visibility override on {}", GetGameVersionName()));

    if (S3SSOwnsVisibilityOverride()) {
        g_externalS3SS = true;
        g_running = true;
        LOG_INFO("[LotVisibility] Official Sims3SettingsSetter owns LotStreamingOptimizations.visibilityOverride; Apex makes no visibility-branch writes");
        return true;
    }

    std::string missing;
    if (!GameAddr::GroupAvailable("LotVisibilityOverride", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }

    ResetState();
    g_address = GameAddr::Get(GameAddr::Id::LotVisibilityCameraBiasJZ);
    if (!g_address) {
        if (error) *error = "The lot visibility camera-bias branch is unavailable";
        return false;
    }

    uint8_t current = 0;
    if (!ReadByte(g_address, current)) {
        if (error) *error = "Could not read the lot visibility camera-bias opcode";
        ResetState();
        return false;
    }

    LOG_INFO(std::format("[LotVisibility] Camera-bias branch at {:#010x}: opcode 0x{:02X}", g_address, current));

    if (current == 0x74) {
        if (!WriteExpectedByte(g_address, 0xEB, 0x74)) {
            if (error) *error = "Could not patch lot visibility camera-bias JZ to JMP";
            ResetState();
            return false;
        }
        g_owned = true;
        LOG_INFO(std::format("[LotVisibility] Visibility override installed (JZ 0x74 -> JMP 0xEB at {:#010x})", g_address));
    } else if (current == 0xEB) {
        g_externalPatched = true;
        LOG_INFO("[LotVisibility] Camera-bias branch is already JMP 0xEB; another owner has already applied the override, Apex leaves it untouched");
    } else {
        if (error) *error = std::format("Unexpected lot visibility opcode 0x{:02X}; expected 0x74 (JZ) or 0xEB (JMP)", current);
        ResetState();
        return false;
    }

    g_externalS3SS = false;
    g_running = true;
    LOG_INFO("[LotVisibility] Camera-bias visibility override active");
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    if (g_externalS3SS) {
        LOG_INFO("[LotVisibility] Off in Apex; Sims3SettingsSetter remains the owner");
    } else if (g_owned && g_address) {
        uint8_t current = 0;
        if (!ReadByte(g_address, current)) {
            LOG_WARNING("[LotVisibility] Restore skipped: visibility opcode is unreadable");
        } else if (current != 0xEB) {
            LOG_INFO(std::format("[LotVisibility] Restore skipped: another owner changed opcode 0xEB to 0x{:02X}", current));
        } else if (WriteExpectedByte(g_address, 0x74, 0xEB)) {
            LOG_INFO(std::format("[LotVisibility] Visibility override restored (JMP 0xEB -> JZ 0x74 at {:#010x})", g_address));
        } else {
            LOG_WARNING("[LotVisibility] Restore failed; current opcode was left unchanged");
        }
    } else if (g_externalPatched) {
        LOG_INFO("[LotVisibility] Apex did not own the existing JMP override; opcode left untouched");
    }

    LOG_INFO("[LotVisibility] Camera-bias visibility override stopped");
    g_running = false;
    g_externalS3SS = false;
    ResetState();
}

void Tick() {
    // Code branch is installed once, like S3SS. No live-setting maintenance is needed here.
}

bool Running() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running;
}

bool HandledByS3SS() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalS3SS;
}

bool AlreadyPatchedExternally() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalPatched;
}

std::string StatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return "Off";
    if (g_externalS3SS) return "Handled by Sims3SettingsSetter";
    if (g_externalPatched) return "On; visibility override already owned externally";
    if (g_owned) return std::format("On; camera-bias JZ->JMP at {:#010x}", g_address);
    return "On";
}

} // namespace LotVisibilityOverride
