#pragma once
// The Apex menu (window id "###ApexWindow", toggled with Ctrl+Shift+F11 by default), in the Violet design: a header
// (logo, product name, status pills, close), a sidebar (Overview, Night Lights, Image, Effects, Display, Developer in
// the development build, Settings) and pages of feature cards. Widgets: ui/widgets.h; icons: ui/icons.h (Lucide);
// style and fonts: ui/violet_theme.h. Layout and wording: docs/ui.md.
#include <string>
#include "overlay.h"

namespace ApexGui {

enum class Startup {
    Loading,          // settings loaded, features wait for the game to settle (first Present + 1 s)
    Running,          // features installed from ApexRadiance.toml
    RefusedOldBuild,  // the old combined S3SS + Apex build is also loaded: features stay off, a banner says so
};

Overlay::Client& Client();
void SetStartup(Startup state, const std::string& detail = {});
Startup GetStartup();
// An older standalone S3SSApex.asi is loaded too (idle, since this build loaded first): features keep running, a banner
// asks to delete it. `module` is its file name.
void SetOldStandaloneNotice(const std::string& module);

} // namespace ApexGui
