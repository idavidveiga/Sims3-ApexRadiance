// Read-only checks: real overlay clock, ImGui FPS averaging and extracted startup gate.
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "framework/overlay_clock.h"
#include "imgui.h"

enum class Startup { Loading, Running, RefusedOldBuild };
std::atomic<Startup> g_startup{Startup::Loading};
struct Patch { bool enabled = true; bool IsEnabled() const { return enabled; } } patch;
const char* kNightLighting = "NightTerrainRelight";
const Patch* Find(const char*) { return &patch; }
bool terrainDrawn = false;
namespace NightLighting { bool WorldLive() { return terrainDrawn; } }
alignas(4) unsigned char worldBytes[0x1B8]{};
uintptr_t worldPointer = reinterpret_cast<uintptr_t>(worldBytes);
namespace GameAddr { enum class Id { WorldManagerPtr }; uintptr_t Get(Id) { return reinterpret_cast<uintptr_t>(&worldPointer); } }
unsigned long long testTick = 1000;
unsigned long long TestTick() { return testTick; }
#define GetTickCount64 TestTick
#include "startup_gate_under_test.inc"
#undef GetTickCount64

int checks = 0;
void Check(bool okay, const char* name) {
    if (!okay) { std::printf("FAIL: %s\n",name); std::exit(1); }
    ++checks;
}
void Mode(int mode) { std::memcpy(worldBytes + 0x1B4, &mode, sizeof(mode)); }
int main() {
    using Clock = Overlay::FrameClock::Clock;
    Overlay::FrameClock clock;
    auto now = Clock::time_point{};
    Check(std::abs(clock.Step(now) - 1.0f/60.0f) < 0.00001f, "first frame has valid delta");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {760,1100};
    unsigned char* atlas = nullptr; int w,h;
    io.Fonts->GetTexDataAsRGBA32(&atlas,&w,&h); io.Fonts->SetTexID(1);
    for (int frame=0;frame<10;++frame) {
        now += std::chrono::milliseconds(7); io.DeltaTime = clock.Step(now);
        ImGui::NewFrame(); ImGui::Render();
    }
    // Menu closed for 70 seconds: the game keeps calling Step but no ImGui frame is drawn.
    for (int frame=0;frame<10000;++frame) { now += std::chrono::milliseconds(7); clock.Step(now); }
    now += std::chrono::milliseconds(7); io.DeltaTime = clock.Step(now);
    ImGui::NewFrame(); ImGui::Render();
    Check(io.Framerate > 140 && io.Framerate < 145, "opening after a long idle does not report false low FPS");
    now += std::chrono::milliseconds(500);
    Check(std::abs(clock.Step(now)-0.5f)<0.00001f, "a real 2 FPS frame is not hidden or clamped");
    Check(clock.Step(now)>0, "identical clock sample remains a valid ImGui delta");
    ImGui::DestroyContext();
    UpdateMenuAvailability(); Check(!g_menuAvailable.load(),"first game load is blocked");
    g_startup = Startup::Running; testTick+=250; UpdateMenuAvailability();
    Check(!g_menuAvailable.load(),"running mod at main menu is blocked");
    worldBytes[0x41] = 1; Mode(0); testTick+=250; UpdateMenuAvailability();
    Check(!g_menuAvailable.load(),"tool/load world mode is blocked");
    Mode(1); testTick+=4000; UpdateMenuAvailability();
    Check(!g_menuAvailable.load(),"active world without terrain drawing is blocked with lighting enabled");
    terrainDrawn = true; testTick+=250; UpdateMenuAvailability();
    Check(!g_menuAvailable.load(),"notices wait for the post-load settle delay");
    testTick+=2900; UpdateMenuAvailability(); Check(!g_menuAvailable.load(),"settle delay is not shortened");
    testTick+=250; UpdateMenuAvailability(); Check(g_menuAvailable.load(),"drawn world unlocks after the delay");
    worldBytes[0x41] = 0; testTick+=250; UpdateMenuAvailability();
    Check(!g_menuAvailable.load(),"unloading immediately revokes cached availability");
    patch.enabled = false; terrainDrawn = false; worldBytes[0x41] = 1; testTick+=250; UpdateMenuAvailability();
    testTick+=3100; UpdateMenuAvailability();
    Check(g_menuAvailable.load(),"lighting disabled uses the documented loaded-session fallback");
    g_startup = Startup::RefusedOldBuild; patch.enabled = true; testTick+=250; UpdateMenuAvailability();
    Check(g_menuAvailable.load(),"compatibility warnings can appear after loading when features are refused");
    worldPointer = 0; testTick+=250; UpdateMenuAvailability();
    Check(!g_menuAvailable.load(),"missing world prevents the menu from opening");
    std::printf("%d overlay clock/startup gate checks passed; no game memory or files accessed\n",checks);
}
