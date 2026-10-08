#pragma once
// Native CASt thumbnail request diagnostics: metadata-only signature tracking.
// Does NOT replay image handles or thumbnail bytes: the game owns native resource lifetimes.
// The embedded x86 Mono InternalCall ABI is unverified. Starts only in explicit developer builds
// compiled with APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS, and is off by default.
// Official UI.dll and game script assemblies remain untouched.
#include <string>

namespace FastCreateAStyle {

// Shared resolver: a second client may monitor additional Mono internal calls without detouring twice.
bool AcquireResolver(std::string* error);
void ReleaseResolver();
bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();
// Read-only counters for the shared CAS ICall resolver pilot.
std::string ResolverStatusText();

} // namespace FastCreateAStyle
