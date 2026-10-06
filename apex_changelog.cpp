// The menu's What's new list (see apex_changelog.h). One short line per change, in the player's words.
#include "apex_changelog.h"
#include <iterator>

namespace ApexChangelog {

namespace {

constexpr const char* k260Added[] = {
    "Sim Occlusion: soft contact shadows on Sims, with separate body and hair strength",
    "A Distance slider for Ambient Occlusion",
    "A welcome page with ready-made profiles: Performance, Default and Quality",
    "An Attention page that appears only when something blocks an effect",
    "Filtered screenshots with F8, saved in the game's Screenshots folder or in Apex Radiance's",
    "Refresh lighting in Lighting > Overview, without Developer mode",
    "What's new: click the version at the bottom of the menu",
    nullptr};
constexpr const char* k260Improved[] = {
    "A cleaner menu: one-line header, footer with the version in the middle",
    "Hold Alt over the menu to hide it completely",
    "Buttons, fields and rows share the same sizes and alignment on every page",
    "Shortcuts: pick a set (letters, numbers or F keys) and change any shortcut",
    "Profiles: the built-in ones are always listed, with 30 new icons",
    "Profiles: choose which settings to apply from a clear selection card",
    "Clearer Lighting page, with fine tuning moved under Advanced",
    "Rooms at Night takes a room color saved in Sims3SettingsSetter into account",
    "Indoor lamps light several stories through stairwells and open floors",
    "Indoor light updates sooner after adding a story, a roof or closing a room",
    "The Optimize rendering switch is gone: Apex renders as it did before 2.5.6",
    "The page and card reset buttons are gone; every control keeps its own default",
    nullptr};
constexpr const char* k260Fixed[] = {
    "Lighting updates after editing lamps and after day and night changes in Build mode",
    "Ground brightness works on every kind of terrain, with no edge at lot borders",
    "Lamp light no longer leaks through walls or into raised rooms on other stories",
    "Depth Blur and the start note wait until the world has finished loading",
    "Effects keep working when the game interface is hidden",
    "Typing in the cheat console no longer triggers Apex shortcuts",
    nullptr};

constexpr const char* k256Added[] = {"Lamp light on alpha-blended sidewalks", nullptr};
constexpr const char* k256Improved[] = {"Rendering optimizations are on by default", nullptr};
constexpr const char* k256Fixed[] = {"Street lamps outside lots update the lighting after their color changes", nullptr};

constexpr Release kReleases[] = {
    {"2.6.0", "2026-10-05", k260Added, k260Improved, k260Fixed},
    {"2.5.6", "2026-10-03", k256Added, k256Improved, k256Fixed},
};

} // namespace

int Count() { return static_cast<int>(std::size(kReleases)); }
const Release& Get(int index) { return kReleases[index < 0 || index >= Count() ? 0 : index]; }

} // namespace ApexChangelog
