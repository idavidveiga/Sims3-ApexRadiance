// The menu's What's new list (see apex_changelog.h). One short line per change, in the player's words.
#include "apex_changelog.h"
#include <iterator>

namespace ApexChangelog {

namespace {

constexpr const char* k260Added[] = {
    "Sim Occlusion: soft contact shadows on Sims, with separate body and hair strength",
    "A welcome page with ready-made profiles: Performance, Default and Quality",
    "An Attention page that appears only when something blocks an effect",
    "What's new: click the version at the bottom of the menu",
    nullptr};
constexpr const char* k260Improved[] = {
    "Filtered screenshots with F8, saved in the game's Screenshots folder or in Apex Radiance's",
    "Menu shortcuts can be changed",
    "Rooms at Night takes a room color saved in Sims3SettingsSetter into account",
    "Hold Alt over the menu to hide it completely",
    "A tidier menu header and footer",
    nullptr};
constexpr const char* k260Fixed[] = {
    "Lighting updates after editing lamps and after day and night changes in Build mode",
    "Ground brightness applies to every terrain shader",
    "Less lamp light leaks between stories through indoor openings",
    "Depth Blur and the start note wait until the world has finished loading",
    nullptr};

constexpr const char* k256Added[] = {"Lamp light on alpha-blended sidewalks", nullptr};
constexpr const char* k256Improved[] = {"Rendering optimizations are on by default", nullptr};
constexpr const char* k256Fixed[] = {"Street lamps outside lots update the lighting after their color changes", nullptr};

constexpr Release kReleases[] = {
    {"2.6.0", "", k260Added, k260Improved, k260Fixed},
    {"2.5.6", "2026-10-03", k256Added, k256Improved, k256Fixed},
};

} // namespace

int Count() { return static_cast<int>(std::size(kReleases)); }
const Release& Get(int index) { return kReleases[index < 0 || index >= Count() ? 0 : index]; }

} // namespace ApexChangelog
