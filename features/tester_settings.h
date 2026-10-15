// Developer Tester tab: the list of its switches, their persistence ([developer.controls.tester]) and defaults
#pragma once
#include "tester_toggles.h"
#include <toml++/toml.hpp>

namespace TesterToggles {
// What a switch needs once changed (the Tester tab runs it)
enum class Effect { None, Rigs, Rooms, Grids };
struct Item {
    const char* key;   // TOML key in [developer.controls.tester]
    std::atomic<bool>* value;
    Effect effect;
};
// Every switch above and every PerfToggles switch, in the Tester tab's order
const Item* Items(int& count);
void SaveDeveloperState(toml::table& t);
void LoadDeveloperState(const toml::table& t);
void RestoreDefaults(); // every switch on (the normal build)
} // namespace TesterToggles
