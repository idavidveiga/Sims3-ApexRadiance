#pragma once
// ApexRadiance.toml: Apex Radiance's own configuration, in Documents\...\Apex Radiance\ (never S3SS.toml).
//   [meta]               version, the build that wrote it, the one-time migration from S3SS.toml
//   [ui]                 toggle_key ("Ctrl+Shift+F11"), font_scale, recommend_s3ss, welcome_done, sidebar_collapsed
//   [display]            mode (borderless window: "off", "borderless_windowed", "borderless_fullscreen")
//   [qol.picture]        Picture filters (same keys as the combined build)
//   [qol.frame_profiler] Frame Profiler (development build)
//   [patches.<Name>]     one table per feature: enabled + its settings (same keys as before the split)
// Writes are atomic (temporary file + replace) and debounced: RequestSave marks the file dirty, the pump thread writes it
// a second later.
#include <windows.h>
#include <string>
#include <vector>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace ApexConfig {

struct KeyChord {
    UINT vk = VK_F11;
    bool ctrl = true;
    bool shift = true;
    bool alt = false;
};

struct UiSettings {
    KeyChord toggle;       // opens / closes the Apex menu (default Ctrl+Shift+F11; S3SS uses a bare Insert)
    float fontScale = 1.0f;
    bool recommendS3SS = true; // the "Recommended: Sims3SettingsSetter" card while S3SS is not loaded ([ui] recommend_s3ss)
    bool welcomeDone = false;  // the welcome tour was finished or skipped ([ui] welcome_done; missing = false, also for migrated configs)
    bool sidebarCollapsed = false; // the sidebar is the icon-only rail ([ui] sidebar_collapsed)
    int language = -1;             // menu language: -1 = Windows' display language, else I18n::Lang ([ui] language = "auto" / "en" / "pt" / "es" / "fr")
};

std::string KeyChordText(const KeyChord& chord); // "Ctrl+Shift+F11"
bool ParseKeyChord(const std::string& text, KeyChord& out);
std::string KeyName(UINT vk);

UiSettings GetUi();
void SetUi(const UiSettings& ui); // and saves (debounced)

// One-time migration (call_once), only while ApexRadiance.toml does not exist:
//  1. the previous standalone build's S3SS\Apex\Apex.toml (same schema) is copied as it is; its apex_imgui.ini is
//     not (the menu's scale changed: the new default window size applies); the old folder stays;
//  2. else the combined build's S3SS.toml: backed up to S3SS.toml.pre-split.bak (in the Apex Radiance folder), then
//     only the settings the features of this build read are carried over (needs PatchManager::CreateAll() first);
//  3. else defaults.
void EnsureMigrated();
std::string MigrationNote(); // what the migration did, for the log and the Settings page (Status > Settings)

// [ui], [display], [qol.*] (no feature is installed here)
void LoadSettings();
// [patches.*]: installs the enabled features, then the ones on by default that the file does not mention
void LoadFeatures();

// Parses ApexRadiance.toml; false when it is missing or unreadable (out is left empty).
bool ReadRoot(toml::table& out);
bool Save(std::string* error = nullptr);
void RequestSave();
void PumpAutosave(); // pump thread
// A save is waiting (a requested save or unsaved feature changes): the menu's status bar says "Saving…"
bool SavePending();

// ---- feature state: looks, profiles and undo (menu, render thread) ----
// The tables ApexRadiance.toml keeps for the features: [patches.<Name>] (every feature, or only the ones a profile
// carries: Night Lights, Every-Story Ground Light, Edge Smoothing, Depth Blur), [qol.picture] and [display].
void CaptureFeatureState(toml::table& out, bool profileFeaturesOnly = false);
// Applies such a table live, like changes in the menu: only the sections that differ from the current state (feature
// settings and on / off through ApexPatch::ApplyTableLive, Picture through SetParams, the window mode through
// Borderless::SetMode); sections the table does not have stay as they are. Marks unsaved changes and requests a save.
void ApplyFeatureState(const toml::table& state);

// ---- profiles: Documents\...\Apex Radiance\Profiles\<name>.toml ----
// Each file is CaptureFeatureState(profile features), limited to the parts chosen when it was saved, plus [meta] (the
// build that wrote it). Loading applies the parts the user picks among those the file has (KeepProfileParts). Names: letters, digits,
// space, - and _ only, at most 32 characters (SanitizeProfileName; empty = not usable).
inline constexpr int kProfileNameMax = 32;
std::string SanitizeProfileName(const std::string& raw);
std::vector<std::string> ListProfiles(); // sorted by name (case-insensitive)
bool ProfileExists(const std::string& name);
// The parts of a profile, as bit flags (index i = bit 1 << i)
enum ProfilePart : unsigned {
    kPartNightLights = 1u << 0,   // Night Lights and Every-Story Ground Light (with Water & Snow)
    kPartColor = 1u << 1,         // Color ([qol] picture)
    kPartDepthBlur = 1u << 2,     // Depth Blur
    kPartEdgeSmoothing = 1u << 3, // Edge Smoothing
    kPartWindow = 1u << 4,        // window mode ([display])
    kPartPerformance = 1u << 5,   // the Performance page's features
};
inline constexpr int kProfilePartCount = 6;
inline constexpr unsigned kProfilePartsAll = (1u << kProfilePartCount) - 1;
const char* ProfilePartName(int index); // English, for the menu ("Night Lights")
unsigned ProfilePartsOf(const toml::table& state);         // the parts a profile table has
void KeepProfileParts(toml::table& state, unsigned parts); // removes the other parts from a profile table
bool SaveProfile(const std::string& name, unsigned parts = kProfilePartsAll, std::string* error = nullptr);
// Parses the profile (does not apply it: see ApplyFeatureState)
bool ReadProfile(const std::string& name, toml::table& out, std::string* error = nullptr);
bool DeleteProfile(const std::string& name, std::string* error = nullptr);
std::wstring ProfilesFolder();  // the Profiles folder inside the Apex Radiance folder (trailing backslash)
bool EnsureProfilesDirectory(); // creates it if needed

} // namespace ApexConfig
