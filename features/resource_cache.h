#pragma once
// Resource lookup cache (Apex Radiance, feature "ResourceLookupCache"; docs/features/performance.md).
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
// Only found keys are remembered (misses always ask the game). Any doubt = the game's own lookup runs.
//
// Hooks: vtable slots only (framework/slot_chain.h), no code bytes. The Frame Profiler's FindProvider counter (dev build)
// is an outer layer of the same slots and keeps counting every call, whichever of the two installs first.
// Thread-safe: lookups run on the render, simulation and loader threads. The table (64k entries, 2.5 MB, allocated once)
// is guarded by an SRW lock that is never held while game code runs.
#include <cstdint>
#include <string>

namespace ResourceCache {

// Installs the hooks and starts answering (feature on) / stops answering and removes them (feature off). Any thread.
bool Start(std::string* error);
void Stop();
bool Running();

// Development build: 1 in `n` cache answers is also checked against the game's own lookup (0 = never); a difference is
// logged and turns the cache off for the session. VerifyAllFor checks every answer for that many seconds.
void SetVerifyEvery(int n);
void VerifyAllFor(double seconds);

// For the Frame Profiler (its FindProvider layer is outside the cache's): what the cache did with the last lookup of the
// calling thread (read and cleared). seen = the cache layer handled the call; hit = answered from the cache; probes =
// packages the cache asked (the answering one + the writable ones above it).
struct LookupNote {
    bool seen = false;
    bool hit = false;
    uint32_t probes = 0;
};
LookupNote TakeLookupNote();

struct Stats {
    uint64_t lookups = 0, hits = 0, misses = 0; // misses = the game's own lookup ran (not found, no longer valid, not cacheable)
    uint64_t bypassed = 0;                      // a package-list change was in progress: passed straight to the game
    uint64_t rejected = 0;                      // a remembered answer failed its re-check (the game's lookup ran instead)
    uint64_t notFound = 0;                      // the game found no package (never remembered)
    uint64_t inserted = 0, notCached = 0;       // answers remembered / not remembered (list moved meanwhile, too many writable packages above...)
    uint64_t probesOnHits = 0;                  // packages asked on cache answers
    uint64_t listChanges = 0, changeNotices = 0, unhookedChanges = 0, fullClears = 0;
    uint64_t verified = 0, mismatches = 0, inconclusive = 0;
    double hitMs = 0.0, missMs = 0.0; // development build: time in cache answers / in the game's own lookup
    uint32_t entries = 0, capacity = 0;
    int listSize = -1, writableProviders = -1; // of the last package-list snapshot (-1 = none yet)
    bool readOnlyClassOk = false;              // the read-only package class was recognised (else nothing can be cached)
    bool selfDisabled = false;
    std::string lastMismatch;
};
Stats GetStats();
// One line for the menu ("On: 97% of lookups answered from the cache ...")
std::string StatusText();
// Development build: status lines, counters, the verification controls
void RenderDeveloperUI();

} // namespace ResourceCache
