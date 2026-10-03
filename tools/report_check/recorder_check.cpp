// Exercise the production request/OnPresent bodies, extracted without rewriting them.
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
bool g_on=false;
std::atomic<bool> g_toggleRequest{false},g_stopRequest{false},g_cancelRequest{false};
DWORD g_startTick=0,g_lastStatus=0,g_savedAt=0;
constexpr DWORD kMaxMs=20000,kStatusEveryMs=100;
constexpr size_t kMaxNotes=40000;
struct Line { DWORD tick;std::string text; };
std::vector<Line> g_lines;
std::string g_settingsAtStart,g_saved;
size_t g_notes=0,g_roomNotes=0;
int starts=0,saves=0,notices=0;
void Start(){g_on=true;g_startTick=GetTickCount();++starts;}
void Stop(){g_on=false;++saves;}
void Status(DWORD){}
#define LOG_INFO(text) ((void)0)
namespace I18n { const char* Tr(const char* text){return text;} }
namespace Captures { void Notify(const std::string&){++notices;} }
namespace Hotkeys {enum class Action{Recorder};std::atomic<bool> pressed{false};bool Take(Action){return pressed.exchange(false);} }
#include "recorder_under_test.inc"
int main(){
    Recorder::RequestStop();Recorder::OnPresent();assert(starts==0&&saves==0&&!Recorder::Active());
    Recorder::RequestCancel();Recorder::OnPresent();assert(starts==0&&saves==0);
    Recorder::RequestToggle();Recorder::OnPresent();assert(starts==1&&Recorder::Active());
    g_lines.push_back({0,"recorded detail"});g_settingsAtStart="settings";g_saved="older saved recording";
    g_notes=g_roomNotes=3;
    g_startTick=GetTickCount()-21000; // cancellation wins even when auto-save is due
    Recorder::RequestCancel();Recorder::RequestStop();Recorder::RequestToggle();Hotkeys::pressed=true;
    Recorder::OnPresent();assert(!Recorder::Active()&&saves==0&&g_lines.empty()&&g_settingsAtStart.empty()&&g_saved.empty()&&g_notes==0&&g_roomNotes==0&&notices==1);
    Recorder::OnPresent();assert(starts==1&&saves==0); // queued toggle and key must not restart it
    Recorder::RequestToggle();Recorder::OnPresent();assert(starts==2);
    Recorder::RequestStop();Recorder::OnPresent();assert(saves==1&&!Recorder::Active());
    Recorder::RequestStop();Recorder::OnPresent();assert(saves==1&&starts==2);
    Recorder::RequestToggle();Recorder::OnPresent();g_startTick=GetTickCount()-21000;
    Recorder::OnPresent();assert(saves==2&&!Recorder::Active());
    Recorder::RequestToggle();Hotkeys::pressed=true;Recorder::OnPresent();assert(starts==4&&Recorder::Active());
    Recorder::OnPresent();assert(starts==4&&saves==2); // simultaneous menu/key input consumed once
    std::puts("10 recording request/cancel/auto-save scenarios passed using production bodies");
}
