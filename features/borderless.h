#pragma once
// Borderless window (Apex Radiance). Display tab, next to Picture and Edge Smoothing.
//  - Off: the game's own window and display mode.
//  - Borderless windowed: no title bar or frame; the window's client area is the back buffer size, centred on its monitor.
//  - Borderless fullscreen: no title bar or frame; the window covers its whole monitor. The back buffer keeps the game's
//    resolution (it is stretched to the window when the two differ).
// How: when the game creates or resets its device, an exclusive-fullscreen request is turned into a windowed one
// (Windowed = TRUE, no refresh rate, DISCARD swap effect, no lockable back buffer); the window's style and position are
// set on the window's own thread (a private message through Apex's window procedure), and set again after every Reset
// and whenever the game changes the style or moves the window (WM_STYLECHANGED / WM_WINDOWPOSCHANGED), with no polling.
// Official S3SS has its own borderless window: when it is loaded with that option configured (S3SS.toml, read-only),
// Apex's stays off, since both would edit the same present parameters.
// Config: [display] mode in ApexRadiance.toml ("off", "borderless_windowed", "borderless_fullscreen"; default off).
#include <windows.h>
#include <d3d9.h>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace Borderless {

enum class Mode : int { Off = 0, Windowed = 1, Fullscreen = 2 };

Mode GetMode();
void SetMode(Mode mode); // saves; applies at once when the game already runs windowed
bool HandledByS3SS();
bool Active(); // a borderless mode is configured and S3SS does not handle it

// D3D9 bootstrap
bool AdjustPresentParams(D3DPRESENT_PARAMETERS* pp, const char* where); // CreateDevice / Reset; true when it changed pp
void OnDevice(HWND window, const D3DPRESENT_PARAMETERS* pp);            // after a successful CreateDevice / Reset
void OnWndProcInstalled();                                               // Apex's window procedure is in: apply now
// Apex's window procedure, before anything else; true = handled (result returned to Windows)
bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

void SaveToToml(toml::table& root); // [display]
void LoadFromToml(const toml::table& root);

// A borderless mode is set but the game still runs in exclusive fullscreen: it applies at the next start
bool NeedsRestart();

void RenderUI(); // the Borderless card's body (menu: Display): mode as a segmented control, restart note

} // namespace Borderless
