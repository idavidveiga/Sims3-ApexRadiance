#pragma once
// Bug-report captures (Apex Radiance; docs/features/bug-reports.md): the recording (F6), the light capture (F7) and the
// lighting snapshot (F8), for players to send with a bug report.
//
// Every capture gets its own folder Documents\...\Apex Radiance\Captures\<YYYY-MM-DD HH-MM-SS> <kind>\ (never reused: a
// second capture in the same second gets " (2)"), and when it is done the folder also gets a copy of ApexRadiance_LOG.txt,
// ApexRadiance.toml, ApexRadiance_Crash.txt when there is one, and "About this capture.txt" (what it is, the version, how
// to send it). Nothing is ever overwritten or deleted by itself; the Report a problem page shows the folder and can delete
// the captures on request. Each start and end shows a note on screen (Notify, drawn by the menu module).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Captures {

std::filesystem::path Root(); // ...\Apex Radiance\Captures
// A new, empty capture folder: "<date> <time> <kind>" (kind = "Recording", "Light capture", "Lighting snapshot")
std::filesystem::path NewFolder(const char* kind);
// The folder is complete: copies the log, the settings and the crash report into it, writes "About this capture.txt"
// (what = one line on what was captured) and shows "Saved" on screen
void Finish(const std::filesystem::path& folder, const std::string& what);
// "<date time> Report": only the log, the settings and the crash report (for any problem, crashes included)
void SaveReport();
// ApexRadiance_Crash.txt was written in the last 7 days: its date and time ("2026-09-30 21:50"), else ""
std::string RecentCrash();

// On-screen note (top-left, the start note's style) for a few seconds; recording = the live recording note
void Notify(const std::string& text, int seconds = 5);
struct Note {
    std::string text;
    bool visible = false;
};
Note CurrentNote(); // render thread: the note to draw now

struct Summary {
    int count = 0;         // capture folders
    uint64_t bytes = 0;    // their size
    std::string newest;    // the newest folder's name
};
Summary Scan();            // walks Captures\ (call at most every few seconds)
void OpenFolder();         // Explorer on Captures\ (created if missing)
int DeleteAll();           // removes every capture folder; the number removed

// Capture sessions (user, 30/09: "sessions that put everything in one place"): while one is open, every capture goes into
// Captures\<date time> Session\<time> <kind>\; EndSession adds the log, the settings and "About this session.txt" (the list
// of its captures) to the session folder. A session left open when the game closes stays as it is (its captures are complete).
void BeginSession();
void EndSession();
bool SessionActive();
int SessionCaptures();       // captures saved in the open session
std::string SessionFolder(); // its folder name ("" when none)

// One capture (or session) folder, for the list on the Report a problem page (newest first)
struct Entry {
    std::string folder; // the folder's name ("2026-09-30 21-50-12 Recording")
    std::string date;   // "2026-09-30"
    std::string time;   // "21:50:12"
    std::string kind;   // "Recording", "Light capture", "Session", ...
    int items = 0;      // a session: the captures in it
    uint64_t bytes = 0;
};
std::vector<Entry> List();               // walks Captures\ (call at most every few seconds)
void Open(const std::string& folder);    // Explorer on that capture
bool Delete(const std::string& folder);  // removes that capture (only folders inside Captures\)

} // namespace Captures
