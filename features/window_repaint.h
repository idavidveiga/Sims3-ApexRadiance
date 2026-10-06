#pragma once
// Lighter window updates (Apex Radiance, feature "WindowRepaint"; Performance page).
//
// The game's window message pump (service 0x00588A00, window vfunc +0x8C = 0x00410890) starts every frame with
// "if (flags & 8 && !paintSuspended) { [+0x1C] = 1; InvalidateRect(hwnd, 0, 0); }" (0x004108A0..0x004108B6; the window
// is created with flags 0x2A). The invalidation makes Windows send a WM_PAINT every frame: it goes through every window
// procedure in the chain, the game's paint handler (0x004109A0: BeginPaint, MonitorFromWindow, EnumDisplayMonitors with
// a FillRect of the other monitors, EndPaint) and a paint event 0x1EE100A that no code listens for (no compare or push
// of that id anywhere in TS3W.exe, 05/10). The picture comes from the D3D swap chain, not from WM_PAINT. The short jump
// over the InvalidateRect call (0x004108AE, 75 0C -> EB 0C) leaves the paints Windows sends by itself (uncovering,
// resizing) exactly as before; [+0x1C] is still set.
#include <string>

namespace WindowRepaint {

bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();

} // namespace WindowRepaint
