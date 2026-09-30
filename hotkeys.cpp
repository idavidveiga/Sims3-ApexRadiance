// Apex's keyboard shortcuts in presets (see hotkeys.h).
#include "hotkeys.h"
#include "build_flavor.h"
#include <atomic>

namespace Hotkeys {
namespace {

using ApexConfig::KeyChord;
constexpr int kActions = static_cast<int>(Action::Count);
std::atomic<bool> g_pending[kActions] = {};

KeyChord Chord(UINT vk) {
    KeyChord c;
    c.vk = vk;
    c.ctrl = true;
    c.shift = true;
    c.alt = false;
    return c;
}

// [preset][action]: Compare, Refresh, Probe, Diagnostics, Recorder, FrameCapture
constexpr UINT kKeys[static_cast<int>(Preset::Count)][kActions] = {
    {'T', 'G', 'V', 'B', 'X', 'F'},
    {'2', '3', '4', '5', '6', '7'},
    {VK_F10, VK_F9, VK_F7, VK_F8, VK_F6, VK_F5},
};
constexpr UINT kMenu[static_cast<int>(Preset::Count)] = {'R', '1', VK_F11};

Preset Current() {
    const int p = ApexConfig::GetUi().hotkeyPreset;
    if (p == kMine) {
        const int b = ApexConfig::GetUi().minePresetBase;
        return b >= 0 && b < static_cast<int>(Preset::Count) ? static_cast<Preset>(b) : Preset::Letters;
    }
    return p >= 0 && p < static_cast<int>(Preset::Count) ? static_cast<Preset>(p) : Preset::FKeys;
}

bool Held(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

} // namespace

KeyChord PresetMenu(Preset p) { return Chord(kMenu[static_cast<int>(p)]); }
KeyChord PresetKey(Preset p, Action a) { return Chord(kKeys[static_cast<int>(p)][static_cast<int>(a)]); }

const char* PresetName(Preset p) {
    switch (p) {
    case Preset::Letters: return "Letters";
    case Preset::Numbers: return "Numbers";
    default: return "F keys";
    }
}
const char* PresetDescription(Preset p) {
    switch (p) {
    case Preset::Letters: return "Keys next to each other on the left: no Fn, one hand, any keyboard layout";
    case Preset::Numbers: return "The easiest to remember: 1 menu, 2 compare, 3 refresh";
    default: return "The keys of earlier versions, for keyboards with F keys";
    }
}

KeyChord Key(Action a) {
    const ApexConfig::UiSettings ui = ApexConfig::GetUi();
    if (a == Action::Compare && ui.compareKey.vk) return ui.compareKey;
    if (a == Action::Refresh && ui.refreshKey.vk) return ui.refreshKey;
    return PresetKey(Current(), a);
}

const char* ActionName(Action a) {
    switch (a) {
    case Action::Compare: return "Compare with the game";
    case Action::Refresh: return "Refresh the lighting";
    case Action::Probe: return "Light Probe";
    case Action::Diagnostics: return "Light Diag";
    case Action::Recorder: return "Lighting recorder";
    default: return "Frame Capture";
    }
}

bool OnKeyDown(WPARAM vk, bool repeat) {
    const bool ctrl = Held(VK_CONTROL), shift = Held(VK_SHIFT), alt = Held(VK_MENU);
    const int last = kPublicBuild ? static_cast<int>(Action::Refresh) : kActions - 1;
    for (int i = 0; i <= last; i++) {
        const KeyChord c = Key(static_cast<Action>(i));
        if (!c.vk || c.vk != vk || c.ctrl != ctrl || c.shift != shift || c.alt != alt) continue;
        if (!repeat) g_pending[i].store(true);
        return true;
    }
    return false;
}

bool Take(Action a) { return g_pending[static_cast<int>(a)].exchange(false); }

} // namespace Hotkeys
