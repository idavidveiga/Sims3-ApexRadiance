#pragma once
// Object lookup index (Apex Radiance, feature "ObjectLookupIndex", menu "Faster object lookups"; experimental, off by
// default; docs/features/performance.md, section "Faster object lookups (C8)").
//
// 0x00C62D40 (Steam 1.67.2), thiscall(worldManager, idLo, idHi, int* visited), is the game's "object by ID": 233 direct
// callers (script natives on the simulation thread, lot lighting, the camera, routing...). It walks the world's object
// tree depth-first (roots at this+0x9C, containers of type 2 "Layer", objects of type 1 "Lot": the only two classes
// derived from the tree's base 0x00C71980) comparing the 64-bit id at +0x48 of every node, and returns the first match
// if it is of type 1. There is no index: every call walks the whole tree (13.6% of the samples of lot lighting hitches).
//
// This feature remembers, per (manager, id), where the game's own walk found the object: the path of indices from the
// root vector down to it and the node pointers and vtables along it. A later lookup re-reads that path from the live
// tree, top-down (so only objects that are in the tree right now are read): each vector still holds the remembered node
// at the remembered index, each node still has its class (vtable), the object's id is still the key and no container on
// the path has taken the key. Then the answer is the object the game's walk would reach through the same path. Any
// difference, an entry older than 2 s, or anything unusual = the game's own walk runs (and its answer is remembered
// again). Only found objects are remembered; the game's misses always walk. The tree's mutators could not all be proven
// (several are non-virtual WorldManager methods), so nothing relies on hooks on them: every answer is validated on use.
// Residual assumption (INFERRED, checked in game by the verification): no second node with the same id appears earlier
// in the walk order while the first one stays in the tree.
//
// Verification (both builds): the first 64 answers of the session and then 1 in 64 are also looked up by the game's walk;
// a different answer (with the path still valid) is logged and turns the feature off for the session.
//
// Hook: framework/entry_chain.h (site ObjectById, layer ObjectIndex; the Frame Profiler's "Object lookup" counter is the
// outer layer). Thread-safe: an SRW lock guards the table (4096 entries, 344 KB, allocated once) and is never held while
// game code runs; the path reads are plain loads under SEH, the same loads the game's walk does.
#include <cstdint>
#include <string>

namespace ObjectIndex {

// Checks the lookup's code and hooks it (feature on) / removes the hook (feature off). Any thread.
bool Start(std::string* error);
void Stop();
bool Running();

// 1 in `n` answers from the index is also checked against the game's walk (0 = only the first 64 of the session).
// VerifyAllFor checks every answer for that many seconds (development build button).
void SetVerifyEvery(int n);
void VerifyAllFor(double seconds);

// For the Frame Profiler (its layer is outside this one): what the index did with the last lookup of the calling thread
// (read and cleared). seen = this layer handled the call; hit = answered from the index.
struct LookupNote {
    bool seen = false;
    bool hit = false;
};
LookupNote TakeLookupNote();

struct Stats {
    uint64_t lookups = 0, hits = 0, walks = 0; // walks = the game's own walk ran (miss, no longer valid, too old)
    uint64_t passed = 0;                       // not handled: visited counter given, id 0, feature off
    uint64_t notFound = 0;                     // the game's walk found nothing (never remembered)
    uint64_t stored = 0, notStored = 0;        // answers remembered / not remembered (unknown class, too deep, path not found)
    uint64_t expired = 0, rejected = 0;        // remembered answers too old / whose path changed
    uint64_t verified = 0, mismatches = 0, inconclusive = 0;
    uint64_t generation = 0;                   // table restarts (a path changed, a start, a check)
    double hitMs = 0.0, walkMs = 0.0;          // development build: time in answers / in the game's walk
    uint32_t entries = 0, capacity = 0;
    uint32_t layerClasses = 0, objectClasses = 0; // vtables recognised as the container / object shapes
    bool selfDisabled = false;
    std::string lastMismatch;
};
Stats GetStats();
std::string StatusText();
// Development build: status lines, counters and the verification controls
void RenderDeveloperUI();

} // namespace ObjectIndex
