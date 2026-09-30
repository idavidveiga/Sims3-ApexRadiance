#pragma once
// Development build: records a few seconds of what the lighting does, with the clock time on every line
// (Ctrl+Shift+F6 starts, again stops; it stops by itself after 20 s). Writes ApexRadiance_Recording_<hhmmss>.txt.
#include <string>
namespace Recorder {
void OnPresent(); // render thread, every frame (Night Lighting's Present)
// For the on-screen note: the seconds recorded so far (-1 = not recording), and the file just saved ("" after 4 s)
int SecondsRecorded();
const char* JustSaved();
// Render thread: whether a recording runs, and a line added to it with the time now (the furniture tracer, the light probe)
bool Active();
void Note(const std::string& text);
}
