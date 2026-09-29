// The Apex menu (see apex_gui.h).
#include "apex_gui.h"
#include "apex_config.h"
#include "apex_version.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "build_flavor.h"
#include "borderless.h"
#include "d3d9_bootstrap.h"
#include "frame_profiler.h"
#include "game_version.h"
#include "night_lighting.h"
#include "patch_base.h"
#include "performance.h"
#include "picture.h"
#include "s3ss_detect.h"
#include "shader_cache.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "imgui.h"
#include <toml++/toml.hpp>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib") // CoInitializeEx for ShellExecuteW (shell32.lib is linked by apex_paths.cpp)

namespace ApexGui {
namespace {

using ApexUi::ButtonKind;
using ApexUi::IconId;
using VioletTheme::Col;

std::atomic<Startup> g_startup{Startup::Loading};
std::mutex g_detailLock;
std::string g_startupDetail;
std::atomic<bool> g_oldStandalone{false}; // an older S3SSApex.asi is loaded too (idle): banner
std::string g_oldStandaloneModule;        // under g_detailLock

// Sidebar pages and the tabs of each page. The selected page and tabs are kept while the game runs (not saved).
enum Page : int { PageOverview, PageLighting, PageWaterSnow, PageColor, PageDepthBlur, PageDisplay, PagePerformance, PageDeveloper, PageSettings };
enum LightingTab : int { LightingLamps, LightingGround, LightingObjects, LightingBuildings };
enum WaterSnowTab : int { WaterTab, SnowTab };
enum DisplayTab : int { DisplayWindow, DisplayAntiAliasing };
enum SettingsTab : int { SettingsMenu, SettingsProfiles, SettingsCompatibility, SettingsAbout };
int g_page = PageOverview;
int g_lightingTab = LightingLamps;
int g_waterSnowTab = WaterTab;
int g_colorTab = Picture::TabBasic;
int g_displayTab = DisplayWindow;
int g_settingsTab = SettingsMenu;

// ---- menu state (render thread, inside the overlay's ImGui frame) ----
char g_search[96] = {};           // the header's search query; non-empty = the content shows the results
bool g_focusSearch = false;       // Ctrl+F: focus the search field this frame
bool g_menuHovered = false;       // the mouse is over the menu window (last frame)
bool g_menuFocused = false;       // the menu window has keyboard focus (last frame)
ImVec2 g_windowMin{}, g_windowMax{}; // the menu window's rectangle (last frame)
std::atomic<bool> g_keysOverMenu{false}; // Alt / B are the menu's (Client::CaptureKey, window thread)
float g_alpha = 1.0f;             // the menu's opacity (peek, slider drag fade)
bool g_waitingForKey = false;     // Settings > Menu: waiting for a new menu key (Esc cancels it, not the menu)
bool g_holdCompare = false;       // the hold-to-compare button is held this frame
bool g_menuEverOpened = false;    // this session (the first-launch hint stops)
bool g_tourChecked = false;       // the welcome tour was considered at the first open of this session
bool g_tourActive = false;
int g_tourStep = 0;
std::atomic<unsigned long long> g_hintUntil{0}; // the first-launch corner hint shows until this tick (GetTickCount64)
std::atomic<bool> g_hintConsidered{false};

// Undo: the feature state at the last click / key activation in the menu (before any widget saw it), and the toast
toml::table g_clickSnapshot;
bool g_haveClickSnapshot = false;
struct Toast {
    bool active = false;
    std::string text;
    toml::table undo; // the state Undo restores
    double start = 0.0;
};
Toast g_toast;
constexpr double kToastSeconds = 4.0;

void ShowToast(const std::string& text, toml::table undo) {
    g_toast.active = true;
    g_toast.text = text;
    g_toast.undo = std::move(undo);
    g_toast.start = ImGui::GetTime();
}

// Opens a page (and one of its tabs)
void Go(int page, int* tabOfPage = nullptr, int tab = 0) {
    g_page = page;
    if (tabOfPage) *tabOfPage = tab;
}

// Descriptions (hover) of the cards that are not ApexPatch features (the patches carry theirs in their metadata)
constexpr const char* kPictureDescription = "Fine-tune how the world looks: brightness, contrast, color, sharpness and smoother skies, plus film-style "
                                            "tones and a vignette. Menus and text keep their normal look. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx";
constexpr const char* kBorderlessDescription = "Play in a window with no title bar or frame, at the game's resolution or filling the whole screen. Part of "
                                               APEX_PRODUCT_NAME ". Credits: @loinyx";
constexpr const char* kShoreDescription = "Ponds and lakes mirror the trees, houses and lamps along their shore, on top of the game's sky reflection. "
                                          "Needs Night Lights and Depth Blur. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx";
constexpr const char* kProfilerDescription = "Measures every frame and breaks down each hitch. Writes ApexRadiance_Hitches.txt. Development build only. Part of "
                                             APEX_PRODUCT_NAME ". Credits: @loinyx";

constexpr const char* kNightLighting = "NightTerrainRelight";
constexpr const char* kUpperFloors = "SplitLevelGroundLight";

// ---- feature state ----

ApexPatch* Find(const char* name) { return PatchManager::Get().Find(name); }

const char* Description(const ApexPatch* patch) {
    const FeatureInfo* meta = patch ? patch->GetMetadata() : nullptr;
    return meta ? meta->description.c_str() : nullptr;
}

bool Loading() { return g_startup.load() != Startup::Running; }

// Switches are inert while features start and on an unsupported game version
bool Switchable(const ApexPatch* patch) { return patch && patch->IsCompatibleWithCurrentVersion() && !Loading(); }

void SetPatch(ApexPatch* patch, bool on) {
    if (on ? patch->Install() : patch->Uninstall()) PatchManager::Get().SetUnsavedChanges(true);
}

void CardNote(const char* text) { ApexUi::IconNote(IconId::Info, text); }

void CardError(const std::string& error) {
    if (error.empty()) return;
    ApexUi::IconNote(IconId::TriangleAlert, ("Error: " + error).c_str(), VioletTheme::kError);
}

void NotAvailableNote(const ApexPatch* patch) { CardNote(patch->UnavailableReason().c_str()); }

// Whether StateNotes draws anything
bool HasStateNotes(const ApexPatch* patch) { return !patch->IsCompatibleWithCurrentVersion() || Loading() || !patch->GetLastError().empty(); }

// "Not available on <version>", "Starting…" and the red error of a feature
void StateNotes(const ApexPatch* patch) {
    if (!patch->IsCompatibleWithCurrentVersion()) NotAvailableNote(patch);
    else if (Loading()) CardNote("Starting\xE2\x80\xA6");
    CardError(patch->GetLastError());
}

// The GPU cost chip of a feature ("~0.4 ms"); nullptr while it is off or not measured
template <int N> const char* CostChip(float ms, char (&buf)[N]) { return ApexUi::CostChipText(ms, buf, N) ? buf : nullptr; }

// One feature card: header (icon, title, subtitle; the description, which ends with the credit, on hover) with its
// on/off switch and its GPU cost chip, then (below a divider) its state notes and body(patch) while it is on. In the
// search results the body is searched even while the feature is off.
template <typename Body> void FeatureCardWith(const char* patchName, IconId icon, const char* title, const char* subtitle, Body&& body) {
    ApexPatch* patch = Find(patchName);
    if (!patch) return;
    ImGui::PushID(patchName);
    if (ApexUi::BeginCard("##Card")) {
        bool on = patch->IsEnabled();
        char chipBuf[24];
        const char* chip = CostChip(patch->GpuCostMs(), chipBuf);
        if (ApexUi::CardHeader(icon, title, subtitle, Description(patch), &on, Switchable(patch), nullptr, chip)) SetPatch(patch, on);
        const bool enabled = patch->IsEnabled();
        if (enabled || HasStateNotes(patch)) ApexUi::CardDivider();
        StateNotes(patch);
        if (enabled || ApexUi::FilterActive()) body(patch);
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// The same with the feature's own controls (RenderCustomUI)
void FeatureCard(const char* patchName, IconId icon, const char* title, const char* subtitle) {
    FeatureCardWith(patchName, icon, title, subtitle, [](ApexPatch* p) { p->RenderCustomUI(); });
}

// A primary "Turn on <feature>" button (inert while the feature cannot be switched); the change can be undone
void TurnOnButton(ApexPatch* patch, const char* label, IconId icon, const char* changeText) {
    ImGui::BeginDisabled(!Switchable(patch));
    if (ApexUi::IconTextButton(label, icon, nullptr, ButtonKind::Primary)) {
        SetPatch(patch, true);
        ApexUi::ReportChange(changeText);
    }
    ImGui::EndDisabled();
}

// Tabs whose options need Night Lights: while it is off, a note and the button to turn it on. True when it is on (and
// always in the search results, where the options are searched even while it is off).
bool NightLightsReady(const char* what) {
    ApexPatch* ntr = Find(kNightLighting);
    if (!ntr) return false;
    if (ntr->IsEnabled() || ApexUi::FilterActive()) return true;
    if (!ntr->IsCompatibleWithCurrentVersion()) {
        NotAvailableNote(ntr);
        return false;
    }
    char note[160];
    std::snprintf(note, sizeof note, "Turn on Night Lights to adjust %s", what);
    CardNote(note);
    TurnOnButton(ntr, "Turn on Night Lights", IconId::MoonStar, "Night Lights turned on");
    return false;
}

// ---- Water Reflections (the shore reflection of Night Lights' lake pass, reflexoNoLago) ----

float g_lastShore = 1.0f; // strength restored when the switch goes back on
constexpr float kShoreDefault = 1.0f; // reflexoNoLago's registered default

bool ShoreOn() { return NightLighting::ShoreReflection() > 0.0f; }

void SetShore(bool on) {
    if (on) {
        NightLighting::SetShoreReflection(g_lastShore > 0.0f ? g_lastShore : kShoreDefault);
    } else {
        const float v = NightLighting::ShoreReflection();
        if (v > 0.0f) g_lastShore = v;
        NightLighting::SetShoreReflection(0.0f);
    }
}

// ---- recommendation of official Sims3SettingsSetter (only while it is not loaded) ----

constexpr const char* kRecommendText = APEX_PRODUCT_NAME " works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame "
                                       "rate limiter, fewer stutters and many extra game settings.";
constexpr const wchar_t* kS3SSReleasesUrl = L"https://github.com/sims3fiend/Sims3SettingsSetter/releases";

bool S3SSMissing() {
    const S3SSDetect::Info info = S3SSDetect::Scan(); // cached after the first scan
    return info.scanned && !info.s3ssLoaded && !info.oldCombinedBuild;
}

// Opens the releases page in the default browser, on a short-lived thread (ShellExecute can take a moment and wants
// COM on its thread; the render thread must not wait for it).
void OpenS3SSReleases() {
    std::thread([] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", kS3SSReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) LOG_WARNING(std::format("[Menu] Could not open the Sims3SettingsSetter releases page ({})", reinterpret_cast<INT_PTR>(r)));
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

void DownloadS3SSButton() {
    if (ApexUi::IconTextButton("Download", IconId::Download, "Opens the Sims3SettingsSetter releases page on GitHub in your browser", ButtonKind::Primary))
        OpenS3SSReleases();
}

void RecommendS3SSCard() {
    if (!S3SSMissing() || !ApexConfig::GetUi().recommendS3SS) return;
    ImGui::PushID("RecommendS3SS");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Puzzle, "Recommended: Sims3SettingsSetter", kRecommendText, nullptr, nullptr);
        ApexUi::Gap(ApexUi::kSpace1);
        DownloadS3SSButton();
        ImGui::SameLine(0.0f, ApexUi::kSpace4 * ApexUi::Unit());
        ImGui::AlignTextToFramePadding();
        if (ImGui::TextLink("Don't show again")) {
            ApexConfig::UiSettings ui = ApexConfig::GetUi();
            ui.recommendS3SS = false; // [ui] recommend_s3ss; Settings > Compatibility keeps the link
            ApexConfig::SetUi(ui);
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- Overview ----

const char* BorderlessModeText() {
    if (Borderless::HandledByS3SS()) return "Sims3SettingsSetter";
    switch (Borderless::GetMode()) {
    case Borderless::Mode::Windowed: return "Window";
    case Borderless::Mode::Fullscreen: return "Fullscreen";
    default: return "Off";
    }
}

void OverviewPatchRow(const char* patchName, IconId icon, const char* name, const char* phrase, int page, int* tabOfPage = nullptr, int tab = 0) {
    ApexPatch* patch = Find(patchName);
    if (!patch) return;
    bool on = patch->IsEnabled(), nameClicked = false;
    char chipBuf[24];
    const char* chip = CostChip(patch->GpuCostMs(), chipBuf);
    if (ApexUi::OverviewRow(patchName, icon, name, phrase, Description(patch), &on, Switchable(patch), nullptr, &nameClicked, chip)) SetPatch(patch, on);
    if (nameClicked) Go(page, tabOfPage, tab);
    CardError(patch->GetLastError());
}

void OverviewPage() {
    ApexUi::PageTitle("Overview", "Everything at a glance; click a name to open its page");
    RecommendS3SSCard();
    ImGui::PushID("Overview");
    if (ApexUi::BeginCard("##Card")) {
        bool nameClicked = false;
        OverviewPatchRow(kNightLighting, IconId::MoonStar, "Night Lights", "Lamps light up your neighborhood at night", PageLighting, &g_lightingTab, LightingLamps);
        if (ApexPatch* ntr = Find(kNightLighting)) {
            ApexPatch* blur = Find("DepthBlur");
            const char* phrase = !ntr->IsEnabled() ? "Needs Night Lights" : (blur && !blur->IsEnabled()) ? "Needs Depth Blur" : "Ponds mirror their shore";
            bool on = ShoreOn();
            if (ApexUi::OverviewRow("WaterReflections", IconId::MirrorRound, "Water Reflections", phrase, kShoreDescription, &on, !Loading(), nullptr, &nameClicked))
                SetShore(on);
            if (nameClicked) Go(PageWaterSnow, &g_waterSnowTab, WaterTab);
        }
        {
            PictureParams p = Picture::Get().GetParams();
            bool on = p.enabled;
            char chipBuf[24];
            const char* chip = p.enabled ? CostChip(Picture::Get().GpuMs(), chipBuf) : nullptr;
            if (ApexUi::OverviewRow("Picture", IconId::Palette, "Picture", "Brightness, color and sharpness", kPictureDescription, &on, true, nullptr, &nameClicked, chip)) {
                p.enabled = on;
                Picture::Get().SetParams(p, true);
            }
            if (nameClicked) Go(PageColor);
        }
        OverviewPatchRow("DepthBlur", IconId::Aperture, "Depth Blur", "Softly blurs the distant background", PageDepthBlur);
        ApexUi::OverviewRow("Borderless", IconId::AppWindow, "Borderless", "Play without a window frame", kBorderlessDescription, nullptr, true, BorderlessModeText(),
                            &nameClicked);
        if (nameClicked) Go(PageDisplay, &g_displayTab, DisplayWindow);
        OverviewPatchRow("EdgeSmoothing", IconId::Spline, "Edge Smoothing", "Clean, smooth edges on the world", PageDisplay, &g_displayTab, DisplayAntiAliasing);
        OverviewPatchRow(Performance::kResourceCacheName, IconId::Gauge, "Faster File Lookups", "Fewer small stutters when things load", PagePerformance);
        OverviewPatchRow(Performance::kLotLightingName, IconId::Gauge, "Lot Lighting While Moving", "Lots relight in small steps as you pan", PagePerformance);
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- World > Lighting ----

// "Upper floors light the ground": the Every-Story Ground Light feature's own switch, inside Ground & Lots
void UpperFloorRow() {
    ApexPatch* patch = Find(kUpperFloors);
    if (!patch) return;
    constexpr const char* kLabel = "Upper floors light the ground";
    constexpr const char* kText = "Lamps upstairs also light the yard below";
    ImGui::PushID("UpperFloors");
    if (NightLighting::SplitLevelProvidedByS3SS()) {
        // Sims3SettingsSetter's own fix does it: shown on, not switchable here
        bool on = true;
        ImGui::BeginDisabled();
        ApexUi::SwitchRow(kLabel, &on, kText);
        ImGui::EndDisabled();
        CardNote("Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)");
    } else {
        bool on = patch->IsEnabled();
        ImGui::BeginDisabled(!Switchable(patch));
        if (ApexUi::SwitchRow(kLabel, &on, kText, patch->IsEnabledByDefault())) SetPatch(patch, on);
        ImGui::EndDisabled();
        if (!patch->IsCompatibleWithCurrentVersion()) NotAvailableNote(patch);
        CardError(patch->GetLastError());
    }
    ImGui::PopID();
}

void LampsTabContent() {
    FeatureCardWith(kNightLighting, IconId::MoonStar, "Night Lights", "Lamps light the ground, objects, walls, roofs, water and snow",
                    [](ApexPatch*) { NightLighting::DrawLampColor(); });
    ApexPatch* ntr = Find(kNightLighting);
    if (!ntr) return;
    if (ntr->IsEnabled()) NightLighting::DrawFooter();
    else if (ntr->IsCompatibleWithCurrentVersion() && !Loading()) CardNote("Turn on Night Lights, then fine-tune each part in the tabs above");
}

void GroundTabContent() {
    if (NightLightsReady("the ground and lots")) NightLighting::DrawGroundCard(&UpperFloorRow);
}

void ObjectsTabContent() {
    if (NightLightsReady("objects")) NightLighting::DrawObjectsCard();
}

void BuildingsTabContent() {
    if (NightLightsReady("walls and roofs")) NightLighting::DrawBuildingsCard();
}

void LightingPage() {
    ApexUi::PageTitle("Lighting", "Warm lamp light around your lots at night");
    static const char* const kTabs[] = {"Lamps", "Ground", "Objects", "Buildings"};
    ApexUi::TabBar("##LightingTabs", &g_lightingTab, kTabs, IM_COUNTOF(kTabs));
    switch (g_lightingTab) {
    case LightingGround: GroundTabContent(); break;
    case LightingObjects: ObjectsTabContent(); break;
    case LightingBuildings: BuildingsTabContent(); break;
    default: LampsTabContent(); break;
    }
}

// ---- World > Water & Snow ----

void WaterReflectionsCard() {
    ApexPatch* ntr = Find(kNightLighting);
    if (!ntr) return;
    ApexPatch* blur = Find("DepthBlur");
    ImGui::PushID("WaterReflections");
    if (ApexUi::BeginCard("##Card")) {
        bool on = ShoreOn();
        if (ApexUi::CardHeader(IconId::MirrorRound, "Water Reflections", "Ponds mirror trees, houses and lamps", kShoreDescription, &on, !Loading())) SetShore(on);
        // The reflection is drawn by Night Lights' water pass and reads Depth Blur's scene depth
        const bool needLights = !ntr->IsEnabled(), needBlur = blur && !blur->IsEnabled();
        if (needLights || needBlur || on) ApexUi::CardDivider();
        if (needLights) {
            CardNote("Needs Night Lights (Lighting page)");
            TurnOnButton(ntr, "Turn on Night Lights", IconId::MoonStar, "Night Lights turned on");
        }
        if (needBlur) {
            CardNote("Needs Depth Blur (Depth Blur page)");
            TurnOnButton(blur, "Turn on Depth Blur", IconId::Aperture, "Depth Blur turned on");
        }
        if (on) {
            float v = NightLighting::ShoreReflection();
            if (ApexUi::SliderPercent("Reflection brightness", &v, 0.05f, 3.0f, "How strong the reflection is; 100% is the default", kShoreDefault)) {
                NightLighting::SetShoreReflection(v);
                g_lastShore = v;
            }
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void WaterTabContent() {
    // The lamp glow needs Night Lights; the Water Reflections card below says so with its own button
    ApexPatch* ntr = Find(kNightLighting);
    if (ntr && (ntr->IsEnabled() || ApexUi::FilterActive())) {
        NightLighting::DrawWaterCard();
    } else if (ntr) {
        CardNote("Lamp glow on ponds needs Night Lights");
        TurnOnButton(ntr, "Turn on Night Lights", IconId::MoonStar, "Night Lights turned on");
        ApexUi::Gap(ApexUi::kSpace2);
    }
    WaterReflectionsCard();
}

void SnowTabContent() {
    if (NightLightsReady("snow")) NightLighting::DrawSnowCard();
}

void WaterSnowPage() {
    ApexUi::PageTitle("Water & Snow", "Ponds, reflections and winter sidewalks");
    static const char* const kTabs[] = {"Water", "Snow"};
    ApexUi::TabBar("##WaterSnowTabs", &g_waterSnowTab, kTabs, IM_COUNTOF(kTabs));
    if (g_waterSnowTab == SnowTab) SnowTabContent();
    else WaterTabContent();
}

// ---- Image > Color ----

// The Picture card above the tabs: its switch ([qol.picture] enabled, saved at once), the GPU cost chip, hold to compare
// (the original picture while held; never saved) and before / after (the left half without Picture; never saved)
void PictureHeaderCard() {
    ImGui::PushID("Picture");
    if (ApexUi::BeginCard("##Card")) {
        PictureParams p = Picture::Get().GetParams();
        bool on = p.enabled, compare = p.compare;
        ApexUi::HeaderExtra extra;
        extra.iconOff = IconId::Columns2;
        extra.iconOn = IconId::Columns2;
        extra.value = &compare;
        extra.enabled = p.enabled;
        extra.tooltip = "Before and after: the left half of the screen without Picture, the right half with it (not saved)";
        extra.holdIcon = IconId::Eye;
        extra.holdTooltip = "Hold to compare: the game without Picture while you hold it (or hold B over the menu)";
        char chipBuf[24];
        const char* chip = p.enabled ? CostChip(Picture::Get().GpuMs(), chipBuf) : nullptr;
        const bool switched = ApexUi::CardHeader(IconId::Palette, "Picture", "Brightness, contrast, color and sharpness", kPictureDescription, &on, true, &extra, chip);
        if (extra.held) g_holdCompare = true;
        if (switched || extra.clicked) {
            p.enabled = on;
            p.compare = compare;
            Picture::Get().SetParams(p, switched);
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void PictureRows(int tab) {
    ImGui::PushID("PictureRows");
    if (ApexUi::BeginCard("##Card")) Picture::Get().RenderUI(tab); // greyed out while Picture is off
    ApexUi::EndCard();
    ImGui::PopID();
}

void ColorPage() {
    ApexUi::PageTitle("Color", "How the game's picture looks");
    PictureHeaderCard();
    static const char* const kTabs[] = {"Basic", "Tones", "Color", "Detail"};
    static_assert(IM_COUNTOF(kTabs) == Picture::TabCount, "one tab name per Picture tab");
    ApexUi::TabBar("##ColorTabs", &g_colorTab, kTabs, IM_COUNTOF(kTabs));
    PictureRows(g_colorTab);
}

// Depth Blur and Edge Smoothing both need the game's own (multisampled) Edge Smoothing off
void GameEdgeSmoothingNote(const char* forWhat) {
    char note[160];
    std::snprintf(note, sizeof note, "For %s, turn off the game's own Edge Smoothing (Options \xE2\x80\xBA Graphics)", forWhat);
    ApexUi::IconNote(IconId::Info, note);
    ApexUi::Gap(ApexUi::kSpace1);
}

// ---- Image > Depth Blur ----

void DepthBlurContent() {
    GameEdgeSmoothingNote("Depth Blur");
    FeatureCard("DepthBlur", IconId::Aperture, "Depth Blur", "Like a camera focused on what you look at");
}

void DepthBlurPage() {
    ApexUi::PageTitle("Depth Blur", "Softly blurs the distant background");
    DepthBlurContent();
}

// ---- System > Display ----

void BorderlessCard() {
    ImGui::PushID("Borderless");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::AppWindow, "Borderless", "Play without a title bar or frame", kBorderlessDescription, nullptr);
        ApexUi::CardDivider();
        Borderless::RenderUI();
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void AntiAliasingContent() {
    GameEdgeSmoothingNote("Edge Smoothing");
    FeatureCard("EdgeSmoothing", IconId::Spline, "Edge Smoothing", "Clean edges on the world; menus stay sharp");
}

void DisplayPage() {
    ApexUi::PageTitle("Display", "The game window and smooth edges");
    static const char* const kTabs[] = {"Window", "Anti-aliasing"};
    ApexUi::TabBar("##DisplayTabs", &g_displayTab, kTabs, IM_COUNTOF(kTabs));
    if (g_displayTab == DisplayAntiAliasing) AntiAliasingContent();
    else BorderlessCard();
}

// ---- System > Performance ----

// A feature shown as one switch row inside a card (its description, which ends with the credit, on hover), with its
// "Not available" / error notes under it. True while it is on (or in the search results, where its rows are searched).
bool FeatureSwitchRow(const char* patchName, const char* label, const char* text) {
    ApexPatch* patch = Find(patchName);
    if (!patch) return false;
    ImGui::PushID(patchName);
    bool on = patch->IsEnabled();
    ImGui::BeginDisabled(!Switchable(patch));
    if (ApexUi::SwitchRow(label, &on, text, patch->IsEnabledByDefault())) SetPatch(patch, on);
    ImGui::EndDisabled();
    if (!ApexUi::FilterActive()) {
        ApexUi::Tooltip(Description(patch));
        if (!patch->IsCompatibleWithCurrentVersion()) NotAvailableNote(patch);
        else if (Loading()) CardNote("Starting\xE2\x80\xA6");
        CardError(patch->GetLastError());
    }
    ImGui::PopID();
    return patch->IsEnabled() || ApexUi::FilterActive();
}

// One card for the performance features (patches/performance.h): the switches and the lot lighting time
void PerformanceCard() {
    ImGui::PushID("Performance");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Gauge, "Performance", "Fewer stutters while you play", nullptr, nullptr);
        ApexUi::CardDivider();
        if (FeatureSwitchRow(Performance::kResourceCacheName, "Faster game file lookups", "Fewer small stutters when objects and textures load"))
            FeatureSwitchRow(Performance::kLookupMissesName, "Remember missing files", "Skips repeated searches for files no package has");
        FeatureSwitchRow(Performance::kFileListName, "Faster file lists", "Fewer stutters when Sims load outfits and shapes");
        if (FeatureSwitchRow(Performance::kLotLightingName, "Spread lot lighting while moving", "Lots relight in small steps while the camera moves")) {
            float ms = static_cast<float>(Performance::LotLightingBudgetMs());
            char value[16];
            std::snprintf(value, sizeof value, "%d ms", Performance::LotLightingBudgetMs());
            ApexUi::SliderOptions o;
            o.tooltip = "The current lot's time per frame while moving; 3 ms is the default";
            o.valueText = value;
            o.leftLabel = "Smoother";
            o.rightLabel = "Lights sooner";
            o.defaultValue = static_cast<float>(Performance::kLotLightingBudgetDefault);
            if (ApexUi::Slider("Lot lighting time while moving", &ms, 1.0f, 15.0f, o)) Performance::SetLotLightingBudgetMs(static_cast<int>(std::lround(ms)));
        }
        FeatureSwitchRow(Performance::kWallShadingName, "Wall shading waits while moving", "Walls of new lots get their shading when you stop");
        if (FeatureSwitchRow(Performance::kFastTextureName, "Faster texture compression", "Fewer hitches when the game builds terrain, Sim and lot textures")) {
            bool cores = Performance::FastTextureSeveralCores();
            if (ApexUi::SwitchRow("Use several cores", &cores, "Large textures are shared out over several processor cores, with the same result", true))
                Performance::SetFastTextureSeveralCores(cores);
        }
        FeatureSwitchRow(Performance::kFastCacheName, "Faster cache compression", "Fewer hitches when the game stores Sims and objects in its caches");
        FeatureSwitchRow(Performance::kSceneBudgetName, "Spread new objects over frames", "Fewer hitches when a lot streams in while the camera moves");
        FeatureSwitchRow(Performance::kObjectIndexName, "Faster object lookups", "Fewer hitches when lot lights update; less script work");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void PerformancePage() {
    ApexUi::PageTitle("Performance", "Fewer stutters while you play");
    PerformanceCard();
}

// ---- System > Developer (development build) ----

// A developer card: header, then body() (or "Off" while the feature is off)
template <typename Body> void DevCard(const char* id, IconId icon, const char* title, const char* subtitle, bool on, Body&& body) {
    ImGui::PushID(id);
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(icon, title, subtitle, nullptr, nullptr);
        ApexUi::CardDivider();
        if (on) body();
        else CardNote("Off");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void DevLightingTab() {
    ApexPatch* ntr = Find(kNightLighting);
    DevCard("DevNightLighting", IconId::MoonStar, "Night Lights", "Status, census, diagnostics and the light probe", ntr && ntr->IsEnabled(),
            [] { NightLighting::DrawDeveloper(); });
    if (ApexPatch* upper = Find(kUpperFloors)) {
        const char* state = !upper->IsEnabled() ? "off" : NightLighting::SplitLevelProvidedByS3SS() ? "on, provided by Sims3SettingsSetter" : "on (GetLotID 0x6BC020 returns 0)";
        ImGui::TextDisabled("Every-Story Ground Light: %s", state);
    }
}

void DevProfilerTab() {
    ImGui::PushID("FrameProfiler");
    if (ApexUi::BeginCard("##Card")) {
        bool on = FrameProfiler::IsEnabled();
        if (ApexUi::CardHeader(IconId::Activity, "Frame Profiler", "Frame times and what each hitch is made of", kProfilerDescription, &on)) {
            FrameProfiler::SetEnabled(on);
            ApexConfig::RequestSave();
        }
        ApexUi::CardDivider();
        FrameProfiler::RenderUI(false); // also while off: the data collected so far stays visible until Clear
    }
    ApexUi::EndCard();
    ImGui::PopID();
    // Background precompile of Apex's own HLSL shaders (shader_cache.h): done, how long, any render-thread wait
    ImGui::TextDisabled("Apex shaders: %s", ShaderCache::StatusText().c_str());
    // Performance features: cache counters and checks, lot lighting budget state (docs/features/performance.md)
    ApexPatch* cache = Find(Performance::kResourceCacheName);
    ApexPatch* lists = Find(Performance::kFileListName);
    ApexPatch* lots = Find(Performance::kLotLightingName);
    ApexPatch* walls = Find(Performance::kWallShadingName);
    ApexPatch* tex = Find(Performance::kFastTextureName);
    ApexPatch* pack = Find(Performance::kFastCacheName);
    ApexPatch* nodes = Find(Performance::kSceneBudgetName);
    ApexPatch* objs = Find(Performance::kObjectIndexName);
    auto on = [](ApexPatch* p) { return p && p->IsEnabled(); };
    const bool anyPerf = on(cache) || on(lists) || on(lots) || on(walls) || on(tex) || on(pack) || on(nodes) || on(objs);
    DevCard("DevPerformance", IconId::Gauge, "Performance",
            "Lookup and file list caches, lot lighting and wall shading while moving, compression, scene nodes, object lookups", anyPerf,
            [cache, lists, lots, walls, tex, pack, nodes, objs, on] {
        // the lookup cache's lines include "Remember missing files" and the file list cache
        if (on(cache) || on(lists)) (cache ? cache : lists)->RenderDeveloperUI();
        else ImGui::TextDisabled("Resource lookup cache and file list cache: off");
        ApexUi::Gap(ApexUi::kSpace2);
        if (on(lots)) lots->RenderDeveloperUI();
        else ImGui::TextDisabled("Lot lighting while moving: off");
        ApexUi::Gap(ApexUi::kSpace2);
        if (on(walls)) walls->RenderDeveloperUI();
        else ImGui::TextDisabled("Wall shading while moving: off");
        ApexUi::Gap(ApexUi::kSpace2);
        if (on(tex)) tex->RenderDeveloperUI();
        else ImGui::TextDisabled("Faster texture compression: off");
        ApexUi::Gap(ApexUi::kSpace2);
        if (on(pack)) pack->RenderDeveloperUI();
        else ImGui::TextDisabled("Faster cache compression: off");
        ApexUi::Gap(ApexUi::kSpace2);
        if (on(nodes)) nodes->RenderDeveloperUI();
        else ImGui::TextDisabled("Spread new objects over frames: off");
        ApexUi::Gap(ApexUi::kSpace2);
        if (on(objs)) objs->RenderDeveloperUI();
        else ImGui::TextDisabled("Faster object lookups: off");
    });
}

void DevDebugViewsTab() {
    ApexPatch* edge = Find("EdgeSmoothing");
    DevCard("DebugEdge", IconId::Bug, "Edge Smoothing", "Status, GPU cost, smoothed pixels in red", edge && edge->IsEnabled(), [edge] { edge->RenderDeveloperUI(); });
    ApexPatch* blur = Find("DepthBlur");
    DevCard("DebugBlur", IconId::Bug, "Depth Blur", "Status, focus, GPU cost, blur amount view", blur && blur->IsEnabled(), [blur] { blur->RenderDeveloperUI(); });
    DevCard("DebugPicture", IconId::Bug, "Picture", "Technical note and GPU cost", true, [] { Picture::Get().RenderDeveloperUI(); });
}

void DeveloperPage() {
    ApexUi::PageTitle("Developer", "Measuring and diagnostic tools (development build only)");
    if (ImGui::BeginTabBar("##DeveloperTabs")) {
        if (ImGui::BeginTabItem("Lighting")) {
            DevLightingTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profiler")) {
            DevProfilerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Capture")) {
            FeatureCard("FrameCapture", IconId::Camera, "Frame Capture", "Every draw call of 2 frames to a text file (Ctrl+Shift+F9)");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug views")) {
            DevDebugViewsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---- System > Settings ----

// Waiting for a new menu key: the first key pressed (with the modifiers held at that moment) becomes the chord.
void MenuKeyRow() {
    constexpr const char* kLabel = "Menu key";
    constexpr const char* kText = "Opens and closes this menu";
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    if (!g_waitingForKey) {
        const std::string key = ApexConfig::KeyChordText(ui.toggle);
        if (!ApexUi::BeginControlRow(kLabel, kText, ImGui::CalcTextSize(key.c_str()).x + gap + ApexUi::ButtonWidth("Change##MenuKey", false))) return;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(key.c_str());
        ImGui::SameLine();
        if (ApexUi::TextButton("Change##MenuKey", "Default is Ctrl+Shift+F11; Insert alone is taken by another mod's menu")) g_waitingForKey = true;
        ApexUi::EndControlRow();
        return;
    }
    constexpr const char* kPrompt = "Press a key (Esc cancels)";
    if (ApexUi::BeginControlRow(kLabel, kText, ImGui::CalcTextSize(kPrompt).x)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Col(VioletTheme::kWarning), "%s", kPrompt);
        ApexUi::EndControlRow();
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_waitingForKey = false;
        return;
    }
    for (UINT vk = 0x08; vk <= 0xFE; vk++) {
        switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_ESCAPE: case VK_RETURN: case VK_SPACE: case VK_TAB: case VK_CAPITAL: case VK_NUMLOCK:
        case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
            continue;
        default: break;
        }
        if (!(GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)) continue;
        ui.toggle.vk = vk;
        ui.toggle.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        ui.toggle.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        ui.toggle.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (vk == VK_INSERT && !ui.toggle.ctrl && !ui.toggle.shift && !ui.toggle.alt) continue; // S3SS's own key
        ApexConfig::SetUi(ui);
        g_waitingForKey = false;
        LOG_INFO("[Menu] Menu key set to " + ApexConfig::KeyChordText(ui.toggle));
        break;
    }
}

// Fixed steps instead of a slider: a slider would move under the mouse while the text it resizes grows
void TextSizeRow() {
    static constexpr float kSizes[] = {0.8f, 0.9f, 1.0f, 1.15f, 1.3f, 1.5f, 1.75f, 2.0f};
    static constexpr const char* kSizeNames[] = {"80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%"};
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    int current = 0;
    for (int i = 0; i < IM_COUNTOF(kSizes); i++)
        if (std::fabs(kSizes[i] - ui.fontScale) < std::fabs(kSizes[current] - ui.fontScale)) current = i;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float stepW = ImGui::GetFrameHeight() * 1.4f; // - and + are equally wide
    const float valueW = ImGui::CalcTextSize("200%").x + 2.0f * ApexUi::kSpace1 * ApexUi::Unit();
    const float resetW = ApexUi::ButtonWidth("Reset##TextSize", false);
    if (!ApexUi::BeginControlRow("Text size", "Makes the whole menu bigger or smaller", 2.0f * stepW + valueW + resetW + 3.0f * gap)) return;
    ImGui::BeginDisabled(current == 0);
    if (ApexUi::TextButton("-##TextSmaller", "Smaller", ButtonKind::Secondary, stepW)) {
        ui.fontScale = kSizes[current - 1];
        ApexConfig::SetUi(ui);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        // The current size, centred between - and +
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::Dummy(ImVec2(valueW, h));
        const ImVec2 ts = ImGui::CalcTextSize(kSizeNames[current]);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (valueW - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), kSizeNames[current]);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(current == IM_COUNTOF(kSizes) - 1);
    if (ApexUi::TextButton("+##TextLarger", "Larger", ButtonKind::Secondary, stepW)) {
        ui.fontScale = kSizes[current + 1];
        ApexConfig::SetUi(ui);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(kSizes[current] == 1.0f);
    if (ApexUi::TextButton("Reset##TextSize", "Back to 100%")) {
        ui.fontScale = 1.0f;
        ApexConfig::SetUi(ui);
    }
    ImGui::EndDisabled();
    ApexUi::EndControlRow();
}

void StartTour() {
    g_tourActive = true;
    g_tourStep = 0;
    g_search[0] = '\0';
}

void WelcomeRow() {
    if (!ApexUi::BeginControlRow("Show the welcome tour again", "Sims3SettingsSetter and your menu key", ApexUi::ButtonWidth("Show##Tour", false))) return;
    if (ApexUi::TextButton("Show##Tour")) StartTour();
    ApexUi::EndControlRow();
}

const std::string& AutosaveHint() {
    static const std::string hint = "Saved to " + ApexUtil::ToUtf8(ApexPaths::ConfigFile());
    return hint;
}

void SaveRow() {
    if (!ApexUi::BeginControlRow("Save settings", "Changes also save by themselves after a second", ApexUi::ButtonWidth("Save now", true))) return;
    if (ApexUi::IconTextButton("Save now", IconId::Save, AutosaveHint().c_str())) ApexConfig::Save();
    ApexUi::EndControlRow();
}

// A status row: label on the left, the value (muted, after an optional small icon) on the right
void InfoRow(const char* label, const std::string& value, IconId icon = IconId::None, unsigned iconRgb = VioletTheme::kTextMuted) {
    const float u = ApexUi::Unit();
    const float is = ApexUi::kIconSmall * u, ig = 6.0f * u;
    const bool withIcon = icon != IconId::None;
    const float valueW = ImGui::CalcTextSize(value.c_str()).x + (withIcon ? is + ig : 0.0f);
    if (!ApexUi::BeginControlRow(label, nullptr, std::min(valueW, ImGui::GetContentRegionAvail().x * 0.6f))) return;
    if (withIcon) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::Dummy(ImVec2(is, h));
        ApexUi::DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x, p.y + (h - is) * 0.5f), is, ImGui::GetColorU32(Col(iconRgb)));
        ImGui::SameLine(0.0f, ig);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ApexUi::EndControlRow();
}

void CreditLine(const char* text) {
    ImGui::Bullet();
    ApexUi::MutedText(text);
}

void MenuTab() {
    ImGui::PushID("Menu");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Settings, "Menu", "Menu key, text size and saving", nullptr, nullptr);
        ApexUi::CardDivider();
        MenuKeyRow();
        TextSizeRow();
        WelcomeRow();
        SaveRow();
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- Settings > Profiles ----

struct ProfilesState {
    char name[ApexConfig::kProfileNameMax + 1] = {};
    std::vector<std::string> list;
    bool listDirty = true;
    std::string message; // the result of the last action
    bool messageError = false;
    std::string confirmDelete;  // a profile waiting for "Delete?"
    std::string confirmReplace; // a name that exists, waiting for "Replace?"
};
ProfilesState g_profiles;

// Only letters, digits, space, - and _ can be typed (the file name is the profile name)
int ProfileNameFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
    return ok ? 0 : 1;
}

void ProfileMessage(const std::string& text, bool error) {
    g_profiles.message = text;
    g_profiles.messageError = error;
}

void SaveProfileNow(const std::string& name) {
    std::string err;
    if (ApexConfig::SaveProfile(name, &err)) {
        ProfileMessage("Saved \"" + name + "\"", false);
        g_profiles.name[0] = '\0';
    } else {
        ProfileMessage("Could not save \"" + name + "\": " + err, true);
    }
    g_profiles.confirmReplace.clear();
    g_profiles.listDirty = true;
}

void LoadProfileNow(const std::string& name) {
    toml::table state;
    std::string err;
    if (!ApexConfig::ReadProfile(name, state, &err)) {
        ProfileMessage("Could not load \"" + name + "\": " + err, true);
        return;
    }
    toml::table before;
    ApexConfig::CaptureFeatureState(before);
    ApexConfig::ApplyFeatureState(state);
    LOG_INFO("[Menu] Profile loaded: " + name);
    ProfileMessage("Loaded \"" + name + "\"", false);
    ShowToast("Profile loaded", std::move(before));
}

void ProfilesTab() {
    ProfilesState& s = g_profiles;
    if (s.listDirty) {
        s.list = ApexConfig::ListProfiles();
        s.listDirty = false;
    }
    const float u = ApexUi::Unit();
    ImGui::PushID("Profiles");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Bookmark, "Profiles", "Save your setup and switch between them", nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::GroupLabel("SAVE CURRENT SETUP");
        ApexUi::MutedText("Night Lights, Color, Depth Blur, Edge Smoothing and window mode");
        const float saveW = ApexUi::ButtonWidth("Save##Profile", true);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::fmax(ImGui::GetContentRegionAvail().x - saveW - gap, 80.0f * u));
        const bool enter = ImGui::InputTextWithHint("##ProfileName", "Profile name", s.name, sizeof s.name,
                                                    ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue, ProfileNameFilter);
        ImGui::SameLine();
        const std::string clean = ApexConfig::SanitizeProfileName(s.name);
        ImGui::BeginDisabled(clean.empty() || Loading());
        const bool save = ApexUi::IconTextButton("Save##Profile", IconId::Save, nullptr, ButtonKind::Primary);
        ImGui::EndDisabled();
        if ((save || (enter && !clean.empty())) && !Loading()) {
            if (ApexConfig::ProfileExists(clean) && s.confirmReplace != clean) s.confirmReplace = clean;
            else SaveProfileNow(clean);
        }
        if (!s.confirmReplace.empty()) {
            const std::string q = "\"" + s.confirmReplace + "\" already exists; replace it?";
            ApexUi::IconNote(IconId::TriangleAlert, q.c_str(), VioletTheme::kWarning);
            ApexUi::Gap(ApexUi::kSpace1);
            if (ApexUi::TextButton("Replace##Profile", nullptr, ButtonKind::Primary)) SaveProfileNow(s.confirmReplace);
            ImGui::SameLine();
            if (ApexUi::TextButton("Cancel##Replace")) s.confirmReplace.clear();
        }
        if (!s.message.empty()) {
            ApexUi::Gap(ApexUi::kSpace1);
            ApexUi::IconNote(s.messageError ? IconId::TriangleAlert : IconId::CircleCheck, s.message.c_str(), s.messageError ? VioletTheme::kError : VioletTheme::kTextMuted);
        }

        ApexUi::GroupLabel("SAVED");
        if (s.list.empty()) ApexUi::MutedText("No profiles yet");
        for (const std::string& name : s.list) {
            ImGui::PushID(name.c_str());
            const bool confirming = s.confirmDelete == name;
            const float loadW = ApexUi::ButtonWidth("Load", true), delW = ApexUi::ButtonWidth("Delete", true);
            const float cancelW = ApexUi::ButtonWidth("Cancel", false);
            const float controlsW = confirming ? delW + gap + cancelW : loadW + gap + delW;
            if (ApexUi::BeginControlRow(name.c_str(), confirming ? "Delete this profile?" : nullptr, controlsW)) {
                if (confirming) {
                    if (ApexUi::IconTextButton("Delete##Confirm", IconId::Trash2, "Deletes the profile file", ButtonKind::Primary)) {
                        std::string err;
                        if (ApexConfig::DeleteProfile(name, &err)) ProfileMessage("Deleted \"" + name + "\"", false);
                        else ProfileMessage("Could not delete \"" + name + "\": " + err, true);
                        s.confirmDelete.clear();
                        s.listDirty = true;
                    }
                    ImGui::SameLine();
                    if (ApexUi::TextButton("Cancel")) s.confirmDelete.clear();
                } else {
                    ImGui::BeginDisabled(Loading());
                    if (ApexUi::IconTextButton("Load", IconId::Download, "Apply this profile; Undo puts your settings back")) LoadProfileNow(name);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ApexUi::IconTextButton("Delete", IconId::Trash2)) s.confirmDelete = name;
                }
                ApexUi::EndControlRow();
            }
            ImGui::PopID();
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void CompatibilityTab() {
    ImGui::PushID("Compatibility");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Puzzle, "Compatibility", "Game version, other mods and settings", nullptr, nullptr);
        ApexUi::CardDivider();
        const Startup startup = g_startup.load();
        InfoRow("Game", GetGameVersionName());
        // Detected: a violet check before the value; missing: a muted info mark
        const bool s3ssLoaded = S3SSDetect::Scan().s3ssLoaded;
        InfoRow("Sims3SettingsSetter", s3ssLoaded ? "Installed" : "Not installed", s3ssLoaded ? IconId::CircleCheck : IconId::Info,
                s3ssLoaded ? VioletTheme::kAccent : VioletTheme::kTextMuted);
        InfoRow("Features", startup == Startup::Running ? "Running" : startup == Startup::Loading ? "Starting\xE2\x80\xA6" : "Off (old combined build found)");
        if (S3SSMissing()) { // shown even after the Overview card was dismissed
            ApexUi::GroupLabel("RECOMMENDED");
            ApexUi::MutedText(kRecommendText);
            ApexUi::Gap(ApexUi::kSpace1);
            DownloadS3SSButton();
        }
        if (ApexUi::BeginAdvanced("Details##Compatibility", "Details")) {
            ApexUi::MutedText(("Sims3SettingsSetter: " + S3SSDetect::Summary()).c_str());
            ApexUi::MutedText(("Settings: " + ApexConfig::MigrationNote()).c_str());
            ApexUi::EndAdvanced();
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void AboutTab() {
    ImGui::PushID("About");
    if (ApexUi::BeginCard("##Card")) {
        constexpr const char* kVersionLine = kPublicBuild ? "Version " APEX_VERSION_STRING " \xC2\xB7 Public build"
                                                          : "Version " APEX_VERSION_STRING " \xC2\xB7 Development build, with developer tools";
        ApexUi::CardHeader(IconId::Info, APEX_PRODUCT_NAME " " APEX_PRODUCT_TAGLINE, kVersionLine, nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::GroupLabel("CREDITS");
        CreditLine("Sims3SettingsSetter by sims3fiend: " APEX_PRODUCT_NAME " began as a fork of it, and its framework is still based on its "
                   "design. Huge thanks to sims3fiend! I recommend using both.");
        CreditLine("Edge Smoothing's FXAA mode follows FXAA 3.11 by Timothy Lottes (NVIDIA).");
        CreditLine("Third-party code: Dear ImGui (MIT), Microsoft Detours (MIT), toml++ (MIT), SMAA by Jorge Jimenez et al. (MIT-style, "
                   "see third_party/smaa/LICENSE.txt), Lucide icons (ISC, see third_party/lucide/LICENSE).");
        CreditLine("Every-Story Ground Light (lamps on upper floors lighting the ground) uses a technique from Arro's Split-Level Lighting Fix.");
        CreditLine(APEX_PRODUCT_NAME " by @loinyx.");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void SettingsPage() {
    ApexUi::PageTitle("Settings", "Menu, profiles, compatibility and credits");
    static const char* const kTabs[] = {"Menu", "Profiles", "Compatibility", "About"};
    if (ApexUi::TabBar("##SettingsTabs", &g_settingsTab, kTabs, IM_COUNTOF(kTabs)) && g_settingsTab == SettingsProfiles) g_profiles.listDirty = true;
    switch (g_settingsTab) {
    case SettingsProfiles: ProfilesTab(); break;
    case SettingsCompatibility: CompatibilityTab(); break;
    case SettingsAbout: AboutTab(); break;
    default: MenuTab(); break;
    }
}

// ---- search ----
// Every searchable part of the menu, one per page tab, in sidebar order. In the results each part is drawn in filter
// mode (ApexUi::BeginFilter): only its matching rows, each under the part's breadcrumb, which opens that page and tab.

struct SearchPart {
    const char* crumb;
    int page;
    int* tab; // nullptr = a page without tabs
    int tabIndex;
    void (*draw)();
};

const SearchPart* SearchParts(int& count) {
    static const SearchPart kParts[] = {
        {"Lighting \xE2\x80\xBA Lamps", PageLighting, &g_lightingTab, LightingLamps, LampsTabContent},
        {"Lighting \xE2\x80\xBA Ground", PageLighting, &g_lightingTab, LightingGround, GroundTabContent},
        {"Lighting \xE2\x80\xBA Objects", PageLighting, &g_lightingTab, LightingObjects, ObjectsTabContent},
        {"Lighting \xE2\x80\xBA Buildings", PageLighting, &g_lightingTab, LightingBuildings, BuildingsTabContent},
        {"Water & Snow \xE2\x80\xBA Water", PageWaterSnow, &g_waterSnowTab, WaterTab, WaterTabContent},
        {"Water & Snow \xE2\x80\xBA Snow", PageWaterSnow, &g_waterSnowTab, SnowTab, SnowTabContent},
        {"Color", PageColor, nullptr, 0, PictureHeaderCard},
        {"Color \xE2\x80\xBA Basic", PageColor, &g_colorTab, Picture::TabBasic, [] { PictureRows(Picture::TabBasic); }},
        {"Color \xE2\x80\xBA Tones", PageColor, &g_colorTab, Picture::TabTones, [] { PictureRows(Picture::TabTones); }},
        {"Color \xE2\x80\xBA Color", PageColor, &g_colorTab, Picture::TabColor, [] { PictureRows(Picture::TabColor); }},
        {"Color \xE2\x80\xBA Detail", PageColor, &g_colorTab, Picture::TabDetail, [] { PictureRows(Picture::TabDetail); }},
        {"Depth Blur", PageDepthBlur, nullptr, 0, DepthBlurContent},
        {"Display \xE2\x80\xBA Window", PageDisplay, &g_displayTab, DisplayWindow, BorderlessCard},
        {"Display \xE2\x80\xBA Anti-aliasing", PageDisplay, &g_displayTab, DisplayAntiAliasing, AntiAliasingContent},
        {"Performance", PagePerformance, nullptr, 0, PerformanceCard},
        {"Settings \xE2\x80\xBA Menu", PageSettings, &g_settingsTab, SettingsMenu, MenuTab},
    };
    count = IM_COUNTOF(kParts);
    return kParts;
}

void SearchResults() {
    ApexUi::PageTitle("Search", "Settings that match; click a page name to open it");
    ImGui::PushID("SearchResults");
    int clicked = -1, drawn = 0;
    if (ApexUi::BeginCard("##Card")) {
        int count = 0;
        const SearchPart* parts = SearchParts(count);
        ApexUi::BeginFilter(g_search);
        for (int i = 0; i < count; i++) {
            ImGui::PushID(i);
            ApexUi::SetFilterCrumb(parts[i].crumb, i);
            parts[i].draw();
            ImGui::PopID();
        }
        drawn = ApexUi::EndFilter(&clicked);
        if (drawn == 0) {
            const std::string none = std::string("No settings match \"") + g_search + "\"";
            ApexUi::MutedText(none.c_str());
        }
        if (clicked >= 0 && clicked < count) {
            Go(parts[clicked].page, parts[clicked].tab, parts[clicked].tabIndex);
            g_search[0] = '\0';
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- welcome tour ----

void FinishTour() {
    g_tourActive = false;
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    if (!ui.welcomeDone) {
        ui.welcomeDone = true; // [ui] welcome_done
        ApexConfig::SetUi(ui);
    }
}

void TourPanel() {
    const float u = ApexUi::Unit();
    constexpr int kSteps = 2;
    g_tourStep = std::clamp(g_tourStep, 0, kSteps - 1);
    ImGui::Dummy(ImVec2(0.0f, ApexUi::kSpace4 * u));
    ImGui::PushID("WelcomeTour");
    if (ApexUi::BeginCard("##Card")) {
        // Step dots and "Step n of N"
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float r = 3.5f * u, step = 12.0f * u;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float lineH = ImGui::GetTextLineHeight();
            for (int i = 0; i < kSteps; i++)
                dl->AddCircleFilled(ImVec2(p.x + r + static_cast<float>(i) * step, p.y + lineH * 0.5f), r,
                                    ImGui::GetColorU32(Col(i == g_tourStep ? VioletTheme::kAccent : VioletTheme::kToggleOff)));
            char text[48];
            std::snprintf(text, sizeof text, "Welcome \xC2\xB7 step %d of %d", g_tourStep + 1, kSteps);
            dl->AddText(ImVec2(p.x + static_cast<float>(kSteps) * step + ApexUi::kSpace2 * u, p.y), ImGui::GetColorU32(Col(VioletTheme::kTextMuted)), text);
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, lineH));
            ApexUi::Gap(ApexUi::kSpace1);
        }
        switch (g_tourStep) {
        case 0: {
            ApexUi::CardHeader(IconId::Puzzle, "Sims3SettingsSetter", "A companion mod by sims3fiend", nullptr, nullptr);
            ApexUi::CardDivider();
            if (S3SSDetect::Scan().s3ssLoaded) {
                ApexUi::IconNote(IconId::CircleCheck, "Installed; you're all set", VioletTheme::kAccent);
            } else {
                ApexUi::MutedText(kRecommendText);
                ApexUi::Gap(ApexUi::kSpace1);
                DownloadS3SSButton();
            }
            break;
        }
        default:
            ApexUi::CardHeader(IconId::Keyboard, "Your menu key", "Press it anytime in the game to open this menu", nullptr, nullptr);
            ApexUi::CardDivider();
            MenuKeyRow();
            break;
        }

        // Buttons: Skip (left), Back and Next / Done (right)
        ApexUi::Gap(ApexUi::kSpace3);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const bool last = g_tourStep == kSteps - 1;
        const char* nextLabel = last ? "Done##Tour" : "Next##Tour";
        const float backW = g_tourStep > 0 ? ApexUi::ButtonWidth("Back##Tour", false) + gap : 0.0f;
        const float nextW = ApexUi::ButtonWidth(nextLabel, false);
        const float startX = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
        const float y = ImGui::GetCursorPosY();
        ImGui::AlignTextToFramePadding();
        if (!last && ImGui::TextLink("Skip##Tour")) FinishTour();
        ImGui::SetCursorPos(ImVec2(startX + std::fmax(0.0f, avail - backW - nextW), y));
        if (g_tourStep > 0) {
            if (ApexUi::TextButton("Back##Tour")) {
                g_tourStep--;
            }
            ImGui::SameLine();
        }
        if (ApexUi::TextButton(nextLabel, nullptr, ButtonKind::Primary)) {
            if (last) FinishTour();
            else g_tourStep++;
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- window parts ----

// The search field in the header: a search icon inside, the "Ctrl+F" hint while empty, a clear button while not
void SearchBox(float x, float y, float width) {
    const float u = ApexUi::Unit();
    const float h = ImGui::GetFrameHeight();
    const float is = ApexUi::kIconSmall * u;
    const bool hasText = g_search[0] != '\0';
    const float clearW = hasText ? h : 0.0f;
    ImGui::SetCursorPos(ImVec2(x, y));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * u + is + 6.0f * u, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
    ImGui::SetNextItemWidth(std::fmax(width - clearW, 40.0f * u));
    if (g_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        g_focusSearch = false;
    }
    ImGui::InputTextWithHint("##Search", "Search settings", g_search, sizeof g_search, ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_AutoSelectAll);
    const bool active = ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 muted = ImGui::GetColorU32(Col(VioletTheme::kTextMuted));
    ApexUi::DrawIcon(dl, IconId::Search, ImVec2(p.x + 8.0f * u, p.y + (h - is) * 0.5f), is, active ? ImGui::GetColorU32(Col(VioletTheme::kAccentLight)) : muted);
    if (!hasText && !active) {
        ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
        const char* hint = "Ctrl+F";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        const float hx = p.x + width - ts.x - 10.0f * u;
        if (hx > p.x + 8.0f * u + is + 6.0f * u + ImGui::CalcTextSize("Search settings").x + 8.0f * u) // only when it fits after the placeholder
            dl->AddText(ImVec2(hx, p.y + (h - ts.y) * 0.5f), muted, hint);
        ImGui::PopFont();
    }
    if (hasText) {
        ImGui::SameLine(0.0f, 0.0f);
        if (ApexUi::IconButton("##ClearSearch", IconId::X, "Clear the search (Esc)", false, h / u)) g_search[0] = '\0';
    }
}

// Logo tile, name and tagline (left); the search field, night/day and frame-time pills and close (right), all centred
// on the logo tile. Returns false when the close button was pressed.
bool Header() {
    const float u = ApexUi::Unit();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float startX = ImGui::GetCursorPosX(), startY = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tile = 32.0f * u;

    // Logo tile
    dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), ImGui::GetColorU32(Col(VioletTheme::kAccent)), ApexUi::kSpace2 * u);
    ImGui::PushFont(VioletTheme::BoldFont(), VioletTheme::BaseFontSize() * 1.3f);
    const ImVec2 letter = ImGui::CalcTextSize(APEX_LOGO_LETTER);
    dl->AddText(ImVec2(p.x + (tile - letter.x) * 0.5f, p.y + (tile - letter.y) * 0.5f), IM_COL32_WHITE, APEX_LOGO_LETTER);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(tile, tile));
    ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);

    // Name and tagline, centred on the tile (saving is shown in the status bar)
    const float nameH = ImGui::GetFontSize() * 1.15f, tagH = ImGui::GetFontSize();
    ImGui::SetCursorPosY(startY + std::fmax(0.0f, (tile - nameH - tagH) * 0.5f));
    ImGui::BeginGroup();
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 0.0f);
    ImGui::PushFont(VioletTheme::BoldFont(), VioletTheme::BaseFontSize() * 1.15f);
    ImGui::TextUnformatted(APEX_PRODUCT_NAME);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
    ImGui::TextUnformatted(APEX_PRODUCT_TAGLINE);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const float nameRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + ApexUi::kSpace3 * u;

    // Right side, laid out from the right edge: [search] [night/day] [frame time] [close]
    const float gap = 6.0f * u;
    const float button = 26.0f * u;
    const float closeX = startX + width - button;

    const ImGuiIO& io = ImGui::GetIO();
    char perf[48];
    const float fps = io.Framerate;
    std::snprintf(perf, sizeof perf, "%.1f ms \xC2\xB7 %.0f fps", fps > 0.0f ? 1000.0f / fps : 0.0f, fps);
    const ImVec2 perfSize = ApexUi::PillSize(perf, false);
    const float perfX = closeX - gap - perfSize.x;

    float level = 0.0f;
    const bool haveLevel = NightLighting::MenuNightLevel(level);
    const bool night = level > 0.5f;
    const char* dayText = night ? "Night" : "Day";
    const ImVec2 daySize = ApexUi::PillSize(dayText, true);
    const float dayX = perfX - gap - daySize.x;

    // The search field gets the room left of the pills; narrow windows drop the pills first
    const float minSearch = 110.0f * u, maxSearch = 220.0f * u;
    bool showDay = haveLevel, showPerf = true;
    auto leftEdge = [&] { return showDay ? dayX : showPerf ? perfX : closeX; };
    if (leftEdge() - gap - nameRight < minSearch) showDay = false;
    if (leftEdge() - gap - nameRight < minSearch) showPerf = false;
    const float searchW = std::fmin(leftEdge() - gap - nameRight, maxSearch);
    const bool tourBlocks = g_tourActive;

    if (showDay) {
        ImGui::SetCursorPos(ImVec2(dayX, startY + (tile - daySize.y) * 0.5f));
        ApexUi::Pill(dayText, night, night ? IconId::Moon : IconId::Sun);
        char tip[96];
        std::snprintf(tip, sizeof tip, "How dark the game thinks it is: %.2f (0 is day, 1 is night)", level);
        ApexUi::Tooltip(tip);
    }
    if (showPerf) {
        ImGui::SetCursorPos(ImVec2(perfX, startY + (tile - perfSize.y) * 0.5f));
        ApexUi::Pill(perf, false);
        ApexUi::Tooltip("Frame time and frame rate, averaged over recent frames");
    }
    if (searchW >= 60.0f * u) {
        ImGui::BeginDisabled(tourBlocks);
        SearchBox(leftEdge() - gap - searchW, startY + (tile - ImGui::GetFrameHeight()) * 0.5f, searchW);
        ImGui::EndDisabled();
    }
    bool keepOpen = true;
    ImGui::SetCursorPos(ImVec2(closeX, startY + (tile - button) * 0.5f));
    const std::string closeTip = "Close (Esc); " + ApexConfig::KeyChordText(ApexConfig::GetUi().toggle) + " opens it again";
    if (ApexUi::IconButton("##Close", IconId::X, closeTip.c_str(), false, 26.0f)) keepOpen = false;
    // The next item starts below the tile
    ImGui::SetCursorPos(ImVec2(startX, startY + tile));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return keepOpen;
}

void Sidebar(bool collapsed) {
    struct Item {
        int page;
        IconId icon;
        const char* label;
        const char* group; // a group label starts before this item
    };
    static const Item items[] = {
        {PageOverview, IconId::LayoutDashboard, "Overview", nullptr},
        {PageLighting, IconId::MoonStar, "Lighting", "WORLD"},
        {PageWaterSnow, IconId::WavesHorizontal, "Water & Snow", nullptr},
        {PageColor, IconId::Palette, "Color", "IMAGE"},
        {PageDepthBlur, IconId::Aperture, "Depth Blur", nullptr},
        {PageDisplay, IconId::Monitor, "Display", "SYSTEM"},
        {PagePerformance, IconId::Gauge, "Performance", nullptr},
        {PageDeveloper, IconId::Wrench, "Developer", nullptr},
        {PageSettings, IconId::Settings, "Settings", nullptr},
    };
    const float u = ApexUi::Unit();
    const bool searching = g_search[0] != '\0';
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
    for (const Item& item : items) {
        if (kPublicBuild && item.page == PageDeveloper) continue;
        if (item.group) ApexUi::SidebarGroup(item.group, collapsed);
        if (ApexUi::SidebarItem(item.icon, item.label, g_page == item.page && !searching, collapsed)) {
            g_page = item.page;
            g_search[0] = '\0'; // leaving the search results
        }
    }
    ImGui::PopStyleVar();

    // Footer: the collapse button and (expanded) the version, at the bottom when there is room
    const float bs = 26.0f;
    const float footerY = ImGui::GetWindowHeight() - bs * u - ApexUi::kSpace1 * u;
    if (footerY > ImGui::GetCursorPosY() + ApexUi::kSpace1 * u) {
        ImGui::SetCursorPos(ImVec2(collapsed ? (ImGui::GetWindowWidth() - bs * u) * 0.5f : ApexUi::kSpace1 * u, footerY));
        if (ApexUi::IconButton("##CollapseSidebar", collapsed ? IconId::ChevronsRight : IconId::ChevronsLeft, collapsed ? "Expand the sidebar" : "Collapse the sidebar",
                               false, bs)) {
            ApexConfig::UiSettings ui = ApexConfig::GetUi();
            ui.sidebarCollapsed = !collapsed; // [ui] sidebar_collapsed
            ApexConfig::SetUi(ui);
        }
        if (!collapsed) {
            ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
            const float textY = footerY + (bs * u - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPos(ImVec2(ApexUi::kSpace1 * u + bs * u + ApexUi::kSpace1 * u, textY));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
            ImGui::TextUnformatted("Version " APEX_VERSION_STRING);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }
}

void DrawPage() {
    switch (g_page) {
    case PageLighting: LightingPage(); break;
    case PageWaterSnow: WaterSnowPage(); break;
    case PageColor: ColorPage(); break;
    case PageDepthBlur: DepthBlurPage(); break;
    case PageDisplay: DisplayPage(); break;
    case PagePerformance: PerformancePage(); break;
    case PageDeveloper:
        if constexpr (!kPublicBuild) {
            ApexUi::SetChangeReporting(false); // developer switches are not part of the undoable state
            DeveloperPage();
            ApexUi::SetChangeReporting(true);
        }
        break;
    case PageSettings: SettingsPage(); break;
    default: OverviewPage(); break;
    }
}

// Height of the status bar (the hairline, a gap and one line of small text)
float StatusBarHeight() {
    ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
    const float lineH = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    return lineH + ApexUi::kSpace2 * ApexUi::Unit();
}

// The thin footer: saving state (left), Sims3SettingsSetter (middle), the peek hint (right)
void StatusBar(float height) {
    const float u = ApexUi::Unit();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, std::floor(p.y)), ImVec2(p.x + w, std::floor(p.y) + 1.0f), ImGui::GetColorU32(Col(VioletTheme::kCardBorder)));
    ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
    const float lineH = ImGui::GetTextLineHeight();
    const float y = p.y + height - lineH;
    const float is = 12.0f * u, ig = 5.0f * u;
    const ImU32 muted = ImGui::GetColorU32(Col(VioletTheme::kTextMuted));

    const bool saving = ApexConfig::SavePending();
    const char* left = saving ? "Saving\xE2\x80\xA6" : "All changes saved";
    const float leftW = is + ig + ImGui::CalcTextSize(left).x;
    const bool s3ss = S3SSDetect::Scan().s3ssLoaded;
    const char* middle = s3ss ? "Sims3SettingsSetter detected" : "Sims3SettingsSetter not installed";
    const float middleW = is + ig + ImGui::CalcTextSize(middle).x;
    const char* right = "Hold Alt to peek";
    const float rightW = ImGui::CalcTextSize(right).x;
    const float spacing = ApexUi::kSpace4 * u;
    const bool showRight = leftW + spacing + rightW <= w;
    const bool showMiddle = showRight && leftW + middleW + rightW + 2.0f * spacing <= w;

    ApexUi::DrawIcon(dl, saving ? IconId::Save : IconId::CircleCheck, ImVec2(p.x, y + (lineH - is) * 0.5f), is,
                     saving ? muted : ImGui::GetColorU32(Col(VioletTheme::kSuccess)));
    dl->AddText(ImVec2(p.x + is + ig, y), saving ? muted : ImGui::GetColorU32(Col(VioletTheme::kSuccess, 0.85f)), left);
    if (showMiddle) {
        const float mx = p.x + std::fmax(leftW + spacing, (w - middleW) * 0.5f);
        ApexUi::DrawIcon(dl, s3ss ? IconId::CircleCheck : IconId::Info, ImVec2(mx, y + (lineH - is) * 0.5f), is,
                         s3ss ? ImGui::GetColorU32(Col(VioletTheme::kAccent)) : muted);
        dl->AddText(ImVec2(mx + is + ig, y), muted, middle);
    }
    if (showRight) dl->AddText(ImVec2(p.x + w - rightW, y), muted, right);
    ImGui::PopFont();
}

// The undo toast at the bottom right of the window, above the status bar: "<what changed>  Undo", about 4 s (fading;
// the timer waits while the mouse is on it)
void DrawToast(float bottomY) {
    if (!g_toast.active) return;
    const float u = ApexUi::Unit();
    const double now = ImGui::GetTime();
    double elapsed = now - g_toast.start;
    if (elapsed > kToastSeconds) {
        g_toast.active = false;
        return;
    }
    const float fade = elapsed < 0.15 ? static_cast<float>(elapsed / 0.15) : elapsed > kToastSeconds - 0.6 ? static_cast<float>((kToastSeconds - elapsed) / 0.6) : 1.0f;
    const float padX = ApexUi::kSpace3 * u, padY = ApexUi::kSpace2 * u, gap = ApexUi::kSpace4 * u;
    const float is = ApexUi::kIconSmall * u;
    const char* undoText = "Undo";
    const ImVec2 textSize = ImGui::CalcTextSize(g_toast.text.c_str());
    const float undoW = is + 5.0f * u + ImGui::CalcTextSize(undoText).x;
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 size(padX + textSize.x + gap + undoW + padX, padY + lineH + padY);
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImVec2 pos(std::fmax(winPos.x + ImGui::GetStyle().WindowPadding.x, winPos.x + winSize.x - ImGui::GetStyle().WindowPadding.x - size.x - ApexUi::kSpace2 * u),
                     bottomY - size.y - ApexUi::kSpace2 * u);

    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::fmin(std::fmax(fade, 0.0f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(VioletTheme::kSelectedBg));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(VioletTheme::kAccentDark));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginChild("##UndoToast", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(cp.x + padX, cp.y + padY), ImGui::GetColorU32(Col(VioletTheme::kText)), g_toast.text.c_str());
        const ImVec2 up(cp.x + padX + textSize.x + gap, cp.y + padY);
        ImGui::SetCursorScreenPos(ImVec2(up.x - 4.0f * u, cp.y + padY - 2.0f * u));
        const bool undo = ImGui::InvisibleButton("##Undo", ImVec2(undoW + 8.0f * u, lineH + 4.0f * u), ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered();
        const ImU32 col = ImGui::GetColorU32(Col(hovered ? VioletTheme::kAccentLight : VioletTheme::kAccent));
        ApexUi::DrawIcon(dl, IconId::Undo2, ImVec2(up.x, up.y + (lineH - is) * 0.5f), is, col);
        dl->AddText(ImVec2(up.x + is + 5.0f * u, up.y), col, undoText);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) g_toast.start = now - std::fmin(elapsed, 1.0); // wait while pointed at
        if (undo) {
            ApexConfig::ApplyFeatureState(g_toast.undo);
            LOG_INFO("[Menu] Undo: " + g_toast.text);
            g_toast.active = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

void MainWindow() {
    const float u = ApexUi::Unit();
    if constexpr (kPublicBuild) {
        if (g_page == PageDeveloper) g_page = PageOverview;
    }
    ImGuiIO& io = ImGui::GetIO();
    bool closeRequested = false;

    // ---- keys and pointer, from the previous frame's hover / focus (before any widget sees this frame's input) ----
    const bool dragging = ApexUi::SliderDragging();
    // Peek: Alt held over the menu makes it nearly transparent and inert (never while typing or dragging)
    const bool peek = g_menuHovered && io.KeyAlt && !io.WantTextInput && !dragging && !ImGui::IsAnyItemActive();
    if (g_menuFocused && !peek) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) g_focusSearch = true;
        // Esc: clears the search, then closes the menu (never while a field is being
        // edited or the menu key is being chosen)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !g_waitingForKey && !ImGui::IsAnyItemActive()) {
            if (g_search[0]) g_search[0] = '\0';
            else closeRequested = true;
        }
    }
    // Undo: the feature state before a click or a keyboard activation inside the menu (not while a slider is active: the
    // key that ends a keyboard adjustment must not replace the state from before it)
    const bool pointerInMenu = io.MousePos.x >= g_windowMin.x && io.MousePos.y >= g_windowMin.y && io.MousePos.x < g_windowMax.x && io.MousePos.y < g_windowMax.y;
    const bool activation = g_menuFocused && (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                              ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    if (!peek && !dragging && ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pointerInMenu) || activation)) {
        ApexConfig::CaptureFeatureState(g_clickSnapshot);
        g_haveClickSnapshot = true;
    }
    // Opacity: peek 0.2 (x the disabled alpha of the inert contents = about 0.1), dragging a slider 0.35 (the dragged
    // row stays opaque), else 1; eased over about 0.1 s
    const float target = peek ? 0.2f : dragging ? 0.35f : 1.0f;
    g_alpha += (target - g_alpha) * std::fmin(1.0f, io.DeltaTime * 14.0f);
    if (std::fabs(target - g_alpha) < 0.01f) g_alpha = target;
    ApexUi::SetKeepActiveSliderOpaque(dragging && !peek);

    ImGui::SetNextWindowSize(ImVec2(560.0f * u, 640.0f * u), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f * u, 300.0f * u), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace3 * u, ApexUi::kSpace3 * u));
    bool open = true;
    // "###ApexWindow": its own id (S3SS's window is ###S3SSWindow), stable whatever the visible name
    const bool drawn = ImGui::Begin(APEX_PRODUCT_NAME "###ApexWindow", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                                                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    g_menuHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    g_menuFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    g_windowMin = ImGui::GetWindowPos();
    g_windowMax = ImVec2(g_windowMin.x + ImGui::GetWindowWidth(), g_windowMin.y + ImGui::GetWindowHeight());
    if (drawn) {
        ImGui::BeginDisabled(peek);
        open = Header();
        ImGui::Dummy(ImVec2(0.0f, 1.0f * u));
        ImGui::Separator();

        // Sidebar (fixed width, or the icon rail) and the page (scrolls in its own child), then the status bar
        const bool collapsed = ApexConfig::GetUi().sidebarCollapsed;
        const float statusH = StatusBarHeight();
        const float bodyH = std::fmax(ImGui::GetContentRegionAvail().y - statusH - ImGui::GetStyle().ItemSpacing.y, 60.0f * u);
        const float sidebarW = (collapsed ? 44.0f : 170.0f) * u;
        ImGui::BeginDisabled(g_tourActive); // the tour is modal-like: finish or skip it first
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, bodyH), ImGuiChildFlags_None, 0);
        Sidebar(collapsed);
        ImGui::EndChild();
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 0.0f);
        {
            // Hairline between the sidebar and the page
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 6.0f * u, a.y), ImVec2(a.x + 6.0f * u, a.y + bodyH), ImGui::GetColorU32(Col(VioletTheme::kCardBorder)), 1.0f);
        }
        ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace2 * u, ApexUi::kSpace1 * u));
        ImGui::BeginChild("##Content", ImVec2(0.0f, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
        ImGui::PopStyleVar();
        if (g_tourActive) {
            TourPanel();
        } else if (g_search[0]) {
            SearchResults();
        } else {
            ImGui::PushID(g_page); // each page's widgets get their own ids
            DrawPage();
            ImGui::PopID();
        }
        ImGui::EndChild();
        const float statusTop = ImGui::GetCursorScreenPos().y;
        StatusBar(statusH);
        DrawToast(statusTop);
        ImGui::EndDisabled();

        // Hold to compare: the eye button, or B while the pointer is over the menu (not while typing)
        const bool holdKey = g_menuHovered && !peek && !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_B);
        if ((g_holdCompare || holdKey) && Picture::Get().GetParams().enabled) Picture::Get().HoldBypass();
    }
    g_holdCompare = false;
    ImGui::End();
    ImGui::PopStyleVar(); // Alpha

    // Alt and B are the menu's while the pointer is over it (the game does not see them); not while typing
    g_keysOverMenu.store(g_menuHovered && !io.WantTextInput);

    // The last change of the frame becomes the undo toast (with the state from before the click)
    std::string change;
    if (ApexUi::TakeChange(change) && g_haveClickSnapshot) {
        ShowToast(change, g_clickSnapshot);
        g_haveClickSnapshot = false; // the next change takes a new snapshot at its own click
    }

    if (!open || closeRequested) {
        g_keysOverMenu.store(false);
        Overlay::SetVisible(false);
    }
}

bool BannerNeeded() { return g_startup.load() == Startup::RefusedOldBuild || g_oldStandalone.load(); }

// Old builds found at startup: the combined build (features off) and/or an older standalone S3SSApex.asi (idle).
void Banner() {
    std::string detail, oldModule;
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        detail = g_startupDetail;
        oldModule = g_oldStandaloneModule;
    }
    const bool refused = g_startup.load() == Startup::RefusedOldBuild;
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("##ApexBanner", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        if (refused) {
            ImGui::TextColored(Col(VioletTheme::kError), APEX_PRODUCT_NAME " is off");
            ImGui::TextUnformatted("An old combined build (Sims3SettingsSetter with Apex inside) is also installed:");
            ImGui::TextUnformatted(detail.c_str());
            ImGui::TextUnformatted("Delete that file from Game\\Bin, keep the official Sims3SettingsSetter.asi, then restart the game.");
        }
        if (g_oldStandalone.load()) {
            if (refused) ImGui::Separator();
            ImGui::TextColored(Col(VioletTheme::kWarning), "An older %s is also installed. Delete it from Game\\Bin.", oldModule.c_str());
            ImGui::TextUnformatted("It's the previous version of " APEX_PRODUCT_NAME " and stays idle for now. Keep ApexRadiance.asi and the official "
                                   "Sims3SettingsSetter.asi.");
        }
    }
    ImGui::End();
}

// ---- first-launch hint: "Apex Radiance is ready · press <key>" in the top-right corner for 10 s (never takes input) ----
constexpr unsigned long long kHintMs = 10000;

// Render thread, every frame (Client::AlwaysDraw): starts the hint once, when the features run and the tour was never done
void UpdateHint() {
    if (g_hintConsidered.load() || g_startup.load() != Startup::Running) return;
    g_hintConsidered.store(true);
    if (!g_menuEverOpened && !ApexConfig::GetUi().welcomeDone) g_hintUntil.store(GetTickCount64() + kHintMs);
}

bool HintVisible() { return !g_menuEverOpened && GetTickCount64() < g_hintUntil.load(); }

void Hint() {
    const unsigned long long now = GetTickCount64(), until = g_hintUntil.load();
    const float left = static_cast<float>(until > now ? until - now : 0) / 1000.0f;
    const float fade = std::fmin(1.0f, left / 0.8f); // fades out over the last 0.8 s
    const std::string text = APEX_PRODUCT_NAME " is ready \xC2\xB7 press " + ApexConfig::KeyChordText(ApexConfig::GetUi().toggle);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float u = ApexUi::Unit();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 20.0f * u, vp->Pos.y + 20.0f * u), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace3 * u, ApexUi::kSpace2 * u));
    if (ImGui::Begin("##ApexHint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        const float is = ApexUi::kIconSmall * u;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float lineH = ImGui::GetTextLineHeight();
        ImGui::Dummy(ImVec2(is, lineH));
        ApexUi::DrawIcon(ImGui::GetWindowDrawList(), IconId::Sparkles, ImVec2(p.x, p.y + (lineH - is) * 0.5f), is, ImGui::GetColorU32(Col(VioletTheme::kAccent)));
        ImGui::SameLine(0.0f, 6.0f * u);
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

class GuiClient final : public Overlay::Client {
  public:
    void Draw() override {
        if (BannerNeeded()) Banner();
        if (!Overlay::IsVisible()) {
            if (HintVisible()) Hint();
            return;
        }
        g_menuEverOpened = true;
        if (!g_tourChecked) {
            // The first time the menu opens this session: the welcome tour until it is done or skipped
            g_tourChecked = true;
            if (!ApexConfig::GetUi().welcomeDone) StartTour();
        }
        MainWindow();
    }

    bool AlwaysDraw() override {
        UpdateHint();
        return BannerNeeded() || HintVisible();
    }

    bool IsToggleKey(WPARAM vk) override {
        const ApexConfig::KeyChord c = ApexConfig::GetUi().toggle;
        if (vk != c.vk) return false;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        return ctrl == c.ctrl && shift == c.shift && alt == c.alt;
    }

    float FontScale() override { return ApexConfig::GetUi().fontScale; }

    bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) override { return Borderless::OnWindowMessage(hwnd, msg, wp, lp, result); }

    // Alt (peek) and B (hold to compare) belong to the menu while the pointer is over it
    bool CaptureKey(WPARAM vk) override { return g_keysOverMenu.load() && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == 'B'); }
};

GuiClient g_client;

} // namespace

Overlay::Client& Client() { return g_client; }

void SetStartup(Startup state, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        g_startupDetail = detail;
    }
    g_startup.store(state);
}

Startup GetStartup() { return g_startup.load(); }

void SetOldStandaloneNotice(const std::string& module) {
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        g_oldStandaloneModule = module.empty() ? std::string("S3SSApex.asi") : module;
    }
    g_oldStandalone.store(true);
}

} // namespace ApexGui
