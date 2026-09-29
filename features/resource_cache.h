#pragma once
// Resource lookup cache (Apex Radiance, features "ResourceLookupCache", "ResourceLookupMisses" and "FileListCache";
// docs/features/performance.md).
//
// ResourceMgr::FindProvider (0x004AFFC0 on Steam 1.67.2) answers "which registered package holds this resource key?" by
// asking every package in priority order (about 290 with the user's mods, two critical sections and a hash probe each,
// ~18 us per lookup; 250-290 lookups in a hitch frame). The cache remembers the answer per (resource manager, key) and,
// on a later lookup, re-checks it cheaply instead of asking every package:
//   - the manager's package list is unchanged (generation counter bumped by hooks on RegisterDatabase,
//     SetDatabasePriority and the engine's own "keys of a package changed" notification; plus a fingerprint check);
//   - the remembered package still holds the key (one probe);
//   - no package above it that can gain keys at run time holds it now (one probe each: every package that is not the
//     read-only, lazily closed package class, e.g. memory databases, loose-file folders, writable packages).
// Any doubt = the game's own lookup runs.
//
// "Remember missing files" (ResourceLookupMisses, an extension of the cache, default off):
//   - negative entries: a key no package holds is remembered as "absent" under the same rules (about 36% of lookups are
//     such keys: the resolve function 0x007D8110 retries every miss with the group bit 0x08000000 flipped). An absent
//     answer is re-checked by probing every package that can gain keys; stored only when every read-only package gave a
//     reliable "no" (its key set or its open index was there, see Remember).
//   - write epochs: hooks on the write paths of the database classes whose writes were traced in the disassembly (the
//     writable package DPF and its derived class, the loose-file folder DDF, the base packed stream; see the .cpp) count
//     every change of such a database. An answer whose databases did not change since it was checked needs no probe of
//     them at all; databases of other classes (memory databases, the downloaded-content database, anything unknown) are
//     still probed on every answer.
// "Faster file lists" (FileListCache, default off): ResourceMgr::GetKeyList (slot +0x20; CAS asks it for every key of a
// type, a linear walk of every package's index) remembers, per read-only package and key type, the keys it returned;
// the other packages are still asked every time. Same generation, same order of packages, same keys per package.
//
// Hooks: vtable slots only (framework/slot_chain.h), plus the entry of the DPF's direct record write
// (framework/entry_chain.h). The Frame Profiler's FindProvider and GetKeyList counters (dev build) are outer layers of the
// same slots and keep counting every call.
// Thread-safe: lookups run on the render, simulation and loader threads. The table (64k entries, 2.8 MB, allocated once)
// is guarded by an SRW lock that is never held while game code runs.
#include <cstdint>
#include <string>

namespace ResourceCache {

// Installs the hooks and starts answering (feature on) / stops answering and removes them (feature off). Any thread.
bool Start(std::string* error);
void Stop();
bool Running();

// "Remember missing files": negative entries + write epochs. May be switched while the cache runs (everything stored so
// far is dropped); remembered while the cache is off and applied when it starts.
bool StartMisses(std::string* error);
void StopMisses();
bool MissesRunning();  // switched on (the cache itself may be off)
bool EpochsActive();   // the write hooks of at least one database class are installed

// "Faster file lists": the GetKeyList cache. Independent of the lookup cache (shares the package-list watchers).
bool StartKeyLists(std::string* error);
void StopKeyLists();
bool KeyListsRunning();

// Development build: 1 in `n` cache answers (lookups and file lists) is also checked against the game's own answer (0 =
// never); a difference is logged and turns that cache off for the session. VerifyAllFor checks every answer for that
// many seconds.
void SetVerifyEvery(int n);
void VerifyAllFor(double seconds);

// For the Frame Profiler (its FindProvider layer is outside the cache's): what the cache did with the last lookup of the
// calling thread (read and cleared). seen = the cache layer handled the call; hit = answered from the cache; negative =
// the answer was "no package holds it"; probes = packages the cache asked.
struct LookupNote {
    bool seen = false;
    bool hit = false;
    bool negative = false;
    uint32_t probes = 0;
};
LookupNote TakeLookupNote();

// For the Frame Profiler's GetKeyList layer: the last key list call of the calling thread (read and cleared). seen = the
// cache layer handled it; cachedPackages = packages whose keys came from memory.
struct KeyListNote {
    bool seen = false;
    uint32_t cachedPackages = 0;
};
KeyListNote TakeKeyListNote();

struct Stats {
    uint64_t lookups = 0, hits = 0, misses = 0; // misses = the game's own lookup ran (not found, no longer valid, not cacheable)
    uint64_t bypassed = 0;                      // a package-list change was in progress: passed straight to the game
    uint64_t rejected = 0;                      // a remembered answer failed its re-check (the game's lookup ran instead)
    uint64_t notFound = 0;                      // the game found no package
    uint64_t inserted = 0, notCached = 0;       // answers remembered / not remembered (list moved meanwhile, too many writable packages above...)
    uint64_t probesOnHits = 0;                  // packages asked on cache answers
    uint64_t negHits = 0, negInserted = 0, negUnreliable = 0; // "absent" answered from memory / remembered / not remembered (a read-only package could not answer for sure)
    uint64_t epochHits = 0, epochRefreshes = 0, epochWrites = 0; // answers with no probe of the traced databases / re-validated after a write / writes counted
    uint64_t listChanges = 0, changeNotices = 0, unhookedChanges = 0, fullClears = 0;
    uint64_t verified = 0, mismatches = 0, inconclusive = 0;
    double hitMs = 0.0, missMs = 0.0; // development build: time in cache answers / in the game's own lookup
    uint32_t entries = 0, capacity = 0;
    int listSize = -1, writableProviders = -1, tracedProviders = -1; // of the last package-list snapshot (-1 = none yet)
    bool readOnlyClassOk = false;              // the read-only package class was recognised (else nothing can be cached)
    bool selfDisabled = false;
    std::string lastMismatch;
    std::string epochClasses; // which database classes have their write hooks installed
    // file lists
    uint64_t klCalls = 0, klCached = 0, klPassed = 0, klPackagesFromMemory = 0, klPackagesAsked = 0, klStored = 0, klNotStored = 0;
    uint64_t klVerified = 0, klMismatches = 0, klInconclusive = 0;
    uint32_t klEntries = 0, klKeys = 0;
    bool klSelfDisabled = false;
    std::string klLastMismatch;
};
Stats GetStats();
// One line for the menu ("On: 97% of lookups answered from the cache ...")
std::string StatusText();
std::string MissesStatusText();
std::string KeyListStatusText();
// One line of counters for the Frame Profiler's report
std::string ReportLine();
// Development build: status lines, counters, the verification controls
void RenderDeveloperUI();

} // namespace ResourceCache
