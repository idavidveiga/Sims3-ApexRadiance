// Split-Level / Every-Story Ground Light (part of Night Lighting)
//
// GetLotID (0x006BC020, thiscall(light) -> 64-bit lot id in EDX:EAX) has two callers: the outdoor-room light gather
// (0x006B635D) and the terrain light bake (0x00C294D0, right before the bake's story test at 0x00C294D9). With the lot id
// reported as 0, both treat every lamp like a world lamp: lot lamps on any story light the ground outside the lot and the
// lot's outdoor rooms, so there is no straight light cut at lot edges or between stories. This is the setup v0.1.0 was
// played with (official Sims3SettingsSetter's "Split-Level Lighting Fix" was on); Apex now provides it itself.
//
// Patch: the first 5 bytes of GetLotID become "xor eax,eax; xor edx,edx; ret" (the rest of the function is left as
// is and no longer reached). The original bytes are checked before writing and put back on uninstall, only when Apex
// wrote them. When official S3SS already provides the same fix (its config or the bytes say so), Apex writes nothing.
//
// Part of Apex Radiance. Credits: @loinyx

#include "patch_base.h"
#include "apex_version.h"
#include "night_lighting.h"
#include "s3ss_detect.h"
#include <atomic>
#include <format>

namespace {

std::atomic<bool> g_providedByS3SS{false}; // for the menu (NightLighting::SplitLevelProvidedByS3SS)

constexpr uintptr_t kGetLotId = 0x006BC020;
// mov eax,[ecx+0C0h]; mov edx,[ecx+0C4h]; ret  (TS3W.exe 1.67.2 Steam)
const std::vector<BYTE> kVanilla = {0x8B, 0x81, 0xC0, 0x00, 0x00, 0x00, 0x8B, 0x91, 0xC4, 0x00, 0x00, 0x00, 0xC3};
// xor eax,eax; xor edx,edx; ret
const std::vector<BYTE> kReturnZero = {0x33, 0xC0, 0x33, 0xD2, 0xC3};

} // namespace

class SplitLevelGroundLightPatch : public ApexPatch {
  public:
    SplitLevelGroundLightPatch() : ApexPatch("SplitLevelGroundLight", nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        providedByS3SS_ = false;
        if (g_gameVersion != GameVersion::Steam) return Fail("Needs the Steam version 1.67.2 of the game");
        if (S3SSDetect::SplitLevelFixActive()) {
            // Official S3SS's Split-Level Lighting Fix does the same thing: nothing to write.
            providedByS3SS_ = true;
            g_providedByS3SS = true;
            isEnabled = true;
            LOG_INFO("[SplitLevelGroundLight] Provided by Sims3SettingsSetter (its Split-Level Lighting Fix is on): Apex writes nothing");
            return true;
        }
        if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kGetLotId), kVanilla.data(), kVanilla.size()))
            return Fail("GetLotID differs at 0x6BC020 (different game version or another mod)");
        const std::vector<BYTE> expected(kVanilla.begin(), kVanilla.begin() + static_cast<std::ptrdiff_t>(kReturnZero.size()));
        if (!MemPatch::WriteBytes(kGetLotId, kReturnZero, &written_, &expected)) return Fail("Could not patch GetLotID");
        isEnabled = true;
        LOG_INFO("[SplitLevelGroundLight] Installed at 0x6BC020 (lot lamps light the ground of every story)");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        if (!written_.empty() && !MemPatch::RestoreAll(written_)) return Fail("Could not restore GetLotID");
        providedByS3SS_ = false;
        g_providedByS3SS = false;
        isEnabled = false;
        LOG_INFO("[SplitLevelGroundLight] Uninstalled");
        return true;
    }

    void Update() override { pendingReinstall = false; }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        if (providedByS3SS_) ImGui::TextDisabled("Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)");
        else ImGui::TextDisabled("Lamps on every floor light the ground outside the lot, with no hard edge at the lot border");
    }

  private:
    std::vector<MemPatch::PatchLocation> written_;
    bool providedByS3SS_ = false;
};

bool NightLighting::SplitLevelProvidedByS3SS() { return g_providedByS3SS.load(); }

APEX_REGISTER_FEATURE(SplitLevelGroundLightPatch, {.displayName = "Every-Story Ground Light",
                                            .description = "Lamps on any floor of a lot light the ground and the yard around the house, with no hard edge at "
                                                           "lot borders or between floors. Part of Night Lights in " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                            .category = "Graphics",
                                            .experimental = false,
                                            .enabledByDefault = true,
                                            .supportedVersions = VERSION_STEAM,
                                            .technicalDetails = {"GetLotID (0x6BC020) returns 0: callers 0x6B635D (outdoor-room light gather) and 0xC294D0 (terrain bake).",
                                                                 "Original bytes checked before writing; restored on uninstall only when Apex wrote them.",
                                                                 "Skipped when official S3SS's Split-Level Lighting Fix is already on."}});
