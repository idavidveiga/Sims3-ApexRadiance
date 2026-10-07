#pragma once
// Crash report: an unhandled exception in the game process writes ApexRadiance_Crash.txt (exception, registers, the
// module + offset of the faulting address and of every stack value that points into a module's code, the Apex features
// that were on) and a small minidump ApexRadiance_Crash.dmp in the Apex Radiance folder, then passes the exception on
// (to the game's own filter, else Windows Error Reporting). Two files, overwritten by the next crash.
// The filter runs in a broken process: it uses no heap, only stack buffers and Win32 calls.
#include <string>

namespace CrashReport {

// Sets the unhandled-exception filter (keeps the previous one and calls it after writing the report). Call it once the
// game has started (after its own start-up code, which may set a filter too).
void Install();
// Sets Apex's terminate handler on the calling thread (MSVC keeps it per thread); first line of every Apex thread body
void ThreadStart();
// Takes the filter back when another module replaced it (that one then runs after the report); pump thread, every second
void Refresh();
// The Apex features on right now (one line); the pump thread refreshes it, the report copies it
void SetFeatureLine(const std::string& line);

} // namespace CrashReport
