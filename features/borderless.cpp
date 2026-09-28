// Borderless window (see borderless.h).
#include "borderless.h"
#include "apex_version.h"
#include "apex_config.h"
#include "apex_log.h"
#include "s3ss_detect.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <toml++/toml.hpp>
#include <atomic>
#include <format>
#include <mutex>
#include <string>

namespace Borderless {
namespace {

std::atomic<int> g_mode{static_cast<int>(Mode::Off)};
std::once_flag g_loadOnce;
std::atomic<bool> g_s3ssHandles{false};
std::atomic<bool> g_s3ssChecked{false};
std::atomic<HWND> g_window{nullptr};
std::atomic<UINT> g_bbWidth{0}, g_bbHeight{0};
std::atomic<bool> g_deviceWindowed{false}; // the device currently runs windowed (style changes apply live)
std::atomic<bool> g_wndProcReady{false};
std::atomic<bool> g_applyPosted{false};
bool g_applying = false; // window thread only
UINT g_applyMessage = 0;

constexpr LONG kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
constexpr LONG kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

const char* ModeKey(Mode m) {
    switch (m) {
    case Mode::Windowed: return "borderless_windowed";
    case Mode::Fullscreen: return "borderless_fullscreen";
    default: return "off";
    }
}

void EnsureLoaded() {
    std::call_once(g_loadOnce, [] {
        toml::table root;
        if (ApexConfig::ReadRoot(root)) LoadFromToml(root);
    });
}

void CheckS3SS() {
    const bool handles = S3SSDetect::Rescan().s3ssLoaded && S3SSDetect::S3SSBorderlessConfigured();
    if (handles != g_s3ssHandles.exchange(handles) || !g_s3ssChecked.exchange(true))
        if (handles) LOG_INFO("[Borderless] Official Sims3SettingsSetter has its borderless window configured: Apex's stays off");
}

UINT ApplyMessage() {
    if (!g_applyMessage) g_applyMessage = RegisterWindowMessageW(L"ApexRadiance.Borderless.Apply");
    return g_applyMessage;
}

// Asks the window's own thread to (re)apply the style and position (never from the render thread: SetWindowPos on a
// window of another thread waits for that thread, which may be waiting for the frame).
void RequestApply() {
    const HWND w = g_window.load();
    if (!w || !Active() || !g_deviceWindowed.load()) return;
    if (!g_wndProcReady.load()) return; // applied when the window procedure is installed (OnWndProcInstalled)
    if (!g_applyPosted.exchange(true)) PostMessageW(w, ApplyMessage(), 0, 0);
}

// The rectangle the window should have (window thread)
bool TargetRect(HWND hwnd, RECT& out) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT m = mi.rcMonitor;
    if (static_cast<Mode>(g_mode.load()) == Mode::Fullscreen) {
        out = m;
        return true;
    }
    LONG w = static_cast<LONG>(g_bbWidth.load()), h = static_cast<LONG>(g_bbHeight.load());
    if (w <= 0 || h <= 0) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    const LONG mw = m.right - m.left, mh = m.bottom - m.top;
    const LONG x = m.left + (w < mw ? (mw - w) / 2 : 0), y = m.top + (h < mh ? (mh - h) / 2 : 0);
    out = RECT{x, y, x + w, y + h};
    return true;
}

bool NeedsApply(HWND hwnd) {
    if (IsIconic(hwnd)) return false; // minimised (alt-tab): leave it alone
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & kFrameStyles) return true;
    RECT want{}, have{};
    if (!TargetRect(hwnd, want) || !GetWindowRect(hwnd, &have)) return false;
    return want.left != have.left || want.top != have.top || want.right != have.right || want.bottom != have.bottom;
}

void Apply(HWND hwnd) {
    if (!Active() || IsIconic(hwnd)) return;
    g_applying = true;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE), ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    const LONG newStyle = (style & ~kFrameStyles) | WS_POPUP, newEx = ex & ~kFrameExStyles;
    if (newStyle != style) SetWindowLongW(hwnd, GWL_STYLE, newStyle);
    if (newEx != ex) SetWindowLongW(hwnd, GWL_EXSTYLE, newEx);
    RECT r{};
    if (TargetRect(hwnd, r))
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_applying = false;
    LOG_INFO(std::format("[Borderless] {} applied: {}x{} at ({}, {})", ModeKey(static_cast<Mode>(g_mode.load())), r.right - r.left, r.bottom - r.top, r.left, r.top));
}

} // namespace

Mode GetMode() {
    EnsureLoaded();
    return static_cast<Mode>(g_mode.load());
}

void SetMode(Mode mode) {
    EnsureLoaded();
    g_mode.store(static_cast<int>(mode));
    ApexConfig::RequestSave();
    RequestApply(); // switching between the two borderless modes applies at once; off needs a restart
}

bool HandledByS3SS() { return g_s3ssHandles.load(); }

bool Active() { return static_cast<Mode>(g_mode.load()) != Mode::Off && !g_s3ssHandles.load(); }

bool AdjustPresentParams(D3DPRESENT_PARAMETERS* pp, const char* where) {
    EnsureLoaded();
    if (!g_s3ssChecked.load()) CheckS3SS(); // once at device creation; again at the first Present
    if (!pp || !Active()) return false;
    std::string changes;
    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        changes += " windowed";
    }
    if (pp->SwapEffect != D3DSWAPEFFECT_DISCARD) { // the back buffer may be stretched to the window
        pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
        changes += " discard";
    }
    if (pp->Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) {
        pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
        changes += " no-lockable";
    }
    if (changes.empty()) return false;
    LOG_INFO(std::format("[Borderless] {}: present parameters changed ({} ) for {}", where, changes, ModeKey(static_cast<Mode>(g_mode.load()))));
    return true;
}

void OnDevice(HWND window, const D3DPRESENT_PARAMETERS* pp) {
    if (window) g_window.store(window);
    if (pp) {
        g_deviceWindowed.store(pp->Windowed != FALSE);
        g_bbWidth.store(pp->BackBufferWidth);
        g_bbHeight.store(pp->BackBufferHeight);
    }
    RequestApply();
}

void OnWndProcInstalled() {
    CheckS3SS(); // every ASI is loaded by now
    g_wndProcReady.store(true);
    RequestApply();
}

bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LRESULT* result) {
    if (msg == ApplyMessage() && msg != 0) {
        g_applyPosted.store(false);
        if (Active() && NeedsApply(hwnd)) Apply(hwnd);
        if (result) *result = 0;
        return true;
    }
    // The game changed the style or moved / resized the window: put it back (once the change is done, on this thread)
    if (!g_applying && Active() && g_deviceWindowed.load() && hwnd == g_window.load() &&
        ((msg == WM_STYLECHANGED && wp == static_cast<WPARAM>(GWL_STYLE)) || msg == WM_WINDOWPOSCHANGED || msg == WM_DISPLAYCHANGE)) {
        if (!g_applyPosted.load() && NeedsApply(hwnd)) {
            g_applyPosted.store(true);
            PostMessageW(hwnd, ApplyMessage(), 0, 0);
        }
    }
    return false;
}

void SaveToToml(toml::table& root) {
    toml::table display;
    if (const toml::table* old = root["display"].as_table()) display = *old;
    display.insert_or_assign("mode", ModeKey(static_cast<Mode>(g_mode.load())));
    root.insert_or_assign("display", std::move(display));
}

void LoadFromToml(const toml::table& root) {
    const std::string mode = root["display"]["mode"].value_or(std::string("off"));
    Mode m = Mode::Off;
    if (mode == "borderless_windowed") m = Mode::Windowed;
    else if (mode == "borderless_fullscreen") m = Mode::Fullscreen;
    g_mode.store(static_cast<int>(m));
}

bool NeedsRestart() { return Active() && !g_deviceWindowed.load(); }

void RenderUI() {
    using ApexUi::IconId;
    EnsureLoaded();
    if (HandledByS3SS()) {
        ApexUi::IconNote(IconId::Info, "Handled by Sims3SettingsSetter (its borderless window is on)");
        if (!ApexUi::FilterActive()) // the note is not drawn in the search results
            ApexUi::Tooltip("Both mods change the same display settings, so " APEX_PRODUCT_NAME " leaves this to Sims3SettingsSetter. Change it in that mod's menu");
        return;
    }
    int mode = g_mode.load();
    static const char* const kNames[] = {"Normal", "Borderless window", "Borderless fullscreen"};
    static const IconId kIcons[] = {IconId::None, IconId::AppWindow, IconId::Maximize};
    static const char* const kTips[] = {"The game's own window or fullscreen", "No frame; keeps the game's resolution, centered on screen",
                                        "No frame; fills the whole monitor"};
    if (ApexUi::SegmentedRow("Window mode", "How the game sits on your screen", "##BorderlessMode", &mode, kNames, 3, kTips, kIcons, static_cast<int>(Mode::Off)))
        SetMode(static_cast<Mode>(mode));
    if (NeedsRestart()) ApexUi::IconNote(IconId::TriangleAlert, "Restart the game to apply", VioletTheme::kWarning);
    else ApexUi::IconNote(IconId::Info, "Borderless modes switch instantly; turning borderless on or off may need a restart");
}

} // namespace Borderless
