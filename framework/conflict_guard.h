#pragma once
// Conflict guard (plan PLANO-SEPARACAO.md, section 3 and step 8): one table of the game-code sites Apex patches, used to
//  1. defer game-code installs until official S3SS has loaded its own patches (first Present + ~1 s, or no S3SS);
//  2. refuse up front the known same-byte pairs from S3SS.toml (read-only);
//  3. byte-compare every site at install and name the module a foreign E8/E9 lands in;
//  4. re-verify the sites every second; when overridden: status line, stop, never restore over foreign bytes.
//
// TODO(step 8, conflict guard agent): implement. Only the interface exists so features can already describe their
// sites; nothing calls Verify/Watch yet and every query below reports "no conflict".
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ConflictGuard {

struct Site {
    const char* feature;          // Apex feature that owns the patch (e.g. "NightTerrainRelight")
    uintptr_t address;            // Steam 1.67.2 address
    std::vector<uint8_t> vanilla; // bytes the game has there
};

enum class State { Unknown, Clean, PatchedByApex, ForeignPatch };

struct SiteStatus {
    State state = State::Unknown;
    std::string owner; // module a foreign jump or call lands in ("Sims3SettingsSetter.asi", ...)
};

// Records the sites of a feature (idempotent).
inline void Declare(const std::vector<Site>&) {}
// True when the feature's sites are free to patch now (TODO: deferral, S3SS.toml intent, byte check).
inline bool MayInstall(const char* /*feature*/, std::string* /*reason*/ = nullptr) { return true; }
inline SiteStatus Query(uintptr_t /*address*/) { return {}; }
// Called about once a second from the pump thread (TODO: watchdog).
inline void Tick() {}

} // namespace ConflictGuard
