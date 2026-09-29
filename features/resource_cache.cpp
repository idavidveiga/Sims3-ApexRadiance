// Resource lookup cache (see resource_cache.h and docs/features/performance.md).
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; addresses through framework/game_addresses.h) ----
//   0x004AFFC0 ResourceMgr::FindProvider thiscall(key*, int* priorityOut), ret 8. Takes the manager's mutex (this+0x48),
//              walks the vector [this+0x30, this+0x34) of 8-byte {Database*, int priority} entries (begin / end read once),
//              and for each one unlocks, calls db->vfunc+0x34(key, 0, 1, 6, 1, 0) (OpenRecord with no record and no info
//              asked = "do you hold this key?"), relocks; the first yes wins: *priorityOut = its priority, returns db. None:
//              returns 0 and leaves *priorityOut alone. Reached only through slot +0x40 of the base (0x00FB2DA0) and
//              derived (0x00FFE250) vtables and the wrapper 0x004AFDA0 (slot +0x44, calls +0x40).
//   The list (the only writers of [this+0x30..0x38]; its vector helpers 0x004B1F50 / 0x004B25B0 / 0x004B0B90 have no
//   other callers):
//   0x004B2D00 RegisterDatabase thiscall(bool add, db*, int priority), ret 0xC, slot +0x34 of the base vtable: add =
//              insert before the first entry of lower priority (so the list is sorted by priority, highest first, ties in
//              registration order; duplicates refused through slot +0x38), remove = erase. 0x00736A70 is ResourceSystem's
//              override (slot +0x34 of the derived vtable, same arguments), which calls 0x004B2D00 directly.
//   0x004B2EC0 SetDatabasePriority thiscall(db*, int priority), ret 8, slot +0x3C of both: erase + sorted re-insert.
//   Destructors 0x004B35A0 / 0x007366A0 (-> 0x004B30B0): the list goes with the manager (game exit).
//   0x004B0960 DatabaseChanged thiscall(db*, keyVector*), ret 8, slot +0x4C of both: the engine's own notification that
//              keys of a package changed (it runs FindProvider for each key and tells the resource cache's listeners).
//              Sent by the ResourceSystem's file watcher when a package file changed on disk (0x00734D10: the package's
//              outer object +0xC "file changed?" then GetKeyList, then mgr+0x4C at 0x00734DBF) and by the loose-file
//              folder databases after a rescan (0x004A4160).
//   Package classes (every IDatabase implementation: the constructors that store the interface base 0x00FB21F8):
//   0x00FFE078 the read-only package class that closes idle files (ctor 0x007342F0, inner object at outer+8; registered by
//              the ResourceSystem's path registration 0x00737950 for Resource.cfg packages, i.e. the game's and the mods'
//              .package files). OpenRecord 0x007345D0: with no record / info asked and a key set present
//              ([this+0xB0], kept while the file is closed) it only probes that set under the package's mutex (this+0x40);
//              otherwise it opens the file (slot +0x50, which also drops the key set), calls the base OpenRecord
//              0x0072D470, which accepts only the open-existing dispositions 6 and 3 (it can never create a record), and
//              releases it (slot +0x54). DeleteRecord (slot +0x40, 0x007346A0) calls 0x00624F70 = "return false". So its key
//              set is the file's index: it changes only when the file on disk changes, which the watcher reports through
//              DatabaseChanged (0x00734D10 above).
//   Other classes (vtables 0x00FB2420 loose-file folders, 0x00FB2600, 0x00FFD790 (the base packed stream), 0x00FFD5F8
//              memory databases, 0x01046EE8 social cache) are treated as able to gain keys at any time.
//
// ---- The cache ----
//   Key = (manager, 16-byte resource key). Value = {package, priority, its index in the list, insert tick}. Only found keys
//   (the game's own misses are never stored). Validity:
//     - g_gen: bumped before and after every RegisterDatabase / SetDatabasePriority / DatabaseChanged (hooked through
//       their vtable slots); g_mutating > 0 while one runs: lookups then go straight to the game and nothing is stored.
//       Entries carry a table stamp tied to the generation they were stored under (a new generation = an empty table,
//       without clearing memory).
//     - a snapshot per (manager, generation): list begin / size / fingerprint and the packages that are not of the
//       read-only class (index, pointer). A list that changed without the hooks noticing (begin / size / fingerprint
//       differ under the same generation) bumps the generation and is logged.
//     - on a lookup: the entry at the stored index still holds {package, priority}; the package still holds the key (one
//       OpenRecord probe, exactly as the game asks); none of the non-read-only packages above it holds the key (one probe
//       each; answers with more than 32 such packages above them are not stored); the generation did not move meanwhile;
//       the entry is younger than 60 s. Any failure: the game's own lookup runs (and its answer is stored again).
//   Nothing of Apex is held (no lock) while game code runs. 32-bit counters (relaxed atomics).
//
// Part of Apex Radiance. Credits: @loinyx

#include "resource_cache.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "slot_chain.h"
#include "imgui.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <climits>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>

namespace ResourceCache {
namespace {

using SlotChain::Layer;
using SlotChain::Site;

// ---- table ----
constexpr uint32_t kSlots = 1u << 16; // 64k entries x 40 bytes = 2.5 MB, allocated once, never freed
constexpr uint32_t kMask = kSlots - 1;
constexpr uint32_t kMaxFill = kSlots / 4 * 3; // then the table starts empty again
constexpr int kMaxProbe = 64;                 // linear probing distance
constexpr int kMaxWritableAbove = 32;         // more non-read-only packages above an answer: not stored
constexpr int kMaxRecorded = 64;              // non-read-only packages recorded per snapshot (by index)
constexpr uint32_t kMaxAgeMs = 60000;         // older answers are looked up again
constexpr int kSnapshots = 4;                 // (manager, generation) snapshots kept
constexpr uint32_t kFingerprintEvery = 1024;  // cache answers between two full-list fingerprint checks

struct Entry {
    uint32_t key[4];
    uint32_t mgr;
    uint32_t provider;
    int32_t priority;
    uint32_t index; // of the provider in the list when stored
    uint32_t stamp; // == g_stamp: live; anything else: empty
    uint32_t tick;  // GetTickCount when stored
};
static_assert(sizeof(Entry) == 40, "Entry layout");

struct Snapshot {
    uint32_t gen = 0; // 0 = unused
    uintptr_t mgr = 0;
    uintptr_t begin = 0;
    uint32_t count = 0;
    uint32_t fingerprint = 0;
    int writable = 0; // non-read-only packages in the list
    int recorded = 0; // the first kMaxRecorded of them
    uint16_t idx[kMaxRecorded] = {};
    uint32_t prov[kMaxRecorded] = {};
};

Entry* g_table = nullptr; // written under g_lock (exclusive), read under g_lock (shared)
SRWLOCK g_lock = SRWLOCK_INIT;
uint32_t g_stamp = 1;    // guarded by g_lock
uint32_t g_stampGen = 0; // the generation the current stamp belongs to
uint32_t g_count = 0;    // live entries
Snapshot g_snap[kSnapshots]; // guarded by g_lock
int g_snapNext = 0;

// ---- validity ----
std::atomic<uint32_t> g_gen{2};
std::atomic<int32_t> g_mutating{0};
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
uintptr_t g_readOnlyVtable = 0; // set by Start before the hooks
bool g_readOnlyOk = false;

// ---- control ----
std::mutex g_ctrl;
bool g_started = false;

// ---- statistics (relaxed; 32-bit so the hot path stays one locked add) ----
struct Counter {
    std::atomic<uint32_t> v{0};
    void Add(uint32_t n = 1) { v.fetch_add(n, std::memory_order_relaxed); }
    uint32_t Get() const { return v.load(std::memory_order_relaxed); }
};
Counter c_lookups, c_hits, c_misses, c_bypassed, c_rejected, c_notFound, c_inserted, c_notCached, c_probes, c_listChanges, c_notices, c_unhooked, c_fullClears,
    c_verified, c_mismatches, c_inconclusive, c_fingerprintChecks;
std::atomic<uint64_t> g_hitTicks{0}, g_missTicks{0}; // development build only
double g_qpcMs = 0.0;
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 64};
std::atomic<uint64_t> g_verifyAllUntil{0}; // GetTickCount64
std::mutex g_mismatchLock;
std::string g_lastMismatch; // guarded by g_mismatchLock
std::atomic<bool> g_unhookedLogged{false};

thread_local LookupNote t_note;
thread_local uint32_t t_verifyCount = 0;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

// ---- game calls ----
using FindFn = uint32_t(__fastcall*)(void* mgr, void* edx, const uint32_t* key, int32_t* priorityOut);
using OpenRecordFn = uint8_t(__fastcall*)(void* db, void* edx, const uint32_t* key, uint32_t record, uint32_t access, uint32_t disposition, uint32_t flag, uint32_t info);
using This3Fn = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t);
using This2Fn = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t);

// db->OpenRecord(key, 0, 1, 6, 1, 0), exactly as FindProvider asks (0x004B0002..0x004B000F). Not guarded: a fault here
// is the game's own fault too.
inline bool Holds(uint32_t db, const uint32_t* key) {
    void* const* vt = *reinterpret_cast<void* const* const*>(static_cast<uintptr_t>(db));
    return reinterpret_cast<OpenRecordFn>(vt[0x34 / 4])(reinterpret_cast<void*>(static_cast<uintptr_t>(db)), nullptr, key, 0, 1, 6, 1, 0) != 0;
}

// ---- package list (read without the manager's lock: SEH-guarded, validated by the generation) ----
bool ReadList(uintptr_t mgr, uintptr_t& begin, uint32_t& count) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(mgr + 0x30);
        const uintptr_t e = *reinterpret_cast<const uintptr_t*>(mgr + 0x34);
        if (!b || e < b || ((e - b) & 7) || e - b > 8u * 8192u) return false;
        begin = b;
        count = static_cast<uint32_t>((e - b) / 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t Fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
    return h;
}

// Snapshot of the list: begin, size, fingerprint, the non-read-only packages. False when unreadable.
bool BuildSnapshot(uintptr_t mgr, Snapshot& s) {
    uintptr_t begin = 0;
    uint32_t count = 0;
    if (!ReadList(mgr, begin, count)) return false;
    s.mgr = mgr;
    s.begin = begin;
    s.count = count;
    s.writable = 0;
    s.recorded = 0;
    uint32_t fp = 2166136261u;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t db = p[2 * i], prio = p[2 * i + 1];
            fp = Fnv(Fnv(fp, db), prio);
            const uint32_t vt = db ? *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(db)) : 0;
            if (g_readOnlyOk && vt == g_readOnlyVtable) continue;
            if (s.recorded < kMaxRecorded && i < 0xFFFF) {
                s.idx[s.recorded] = static_cast<uint16_t>(i);
                s.prov[s.recorded] = db;
                s.recorded++;
            }
            s.writable++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    s.fingerprint = fp;
    return true;
}

bool Fingerprint(uintptr_t mgr, uintptr_t& begin, uint32_t& count, uint32_t& fp) {
    if (!ReadList(mgr, begin, count)) return false;
    fp = 2166136261u;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < 2 * count; i++) fp = Fnv(fp, p[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The entry `index` of the list still is {provider, priority}, and the list is where the snapshot saw it
bool ListEntryIs(uintptr_t mgr, uintptr_t begin, uint32_t count, uint32_t index, uint32_t provider, int32_t priority) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(mgr + 0x30);
        const uintptr_t e = *reinterpret_cast<const uintptr_t*>(mgr + 0x34);
        if (b != begin || e < b || (e - b) / 8 != count || index >= count) return false;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(b);
        return p[2 * index] == provider && static_cast<int32_t>(p[2 * index + 1]) == priority;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Where {provider, priority} sits in the list now
bool FindInList(uintptr_t mgr, uint32_t provider, int32_t priority, uint32_t& index) {
    uintptr_t begin = 0;
    uint32_t count = 0;
    if (!ReadList(mgr, begin, count)) return false;
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(begin);
        for (uint32_t i = 0; i < count; i++)
            if (p[2 * i] == provider) {
                index = i;
                return static_cast<int32_t>(p[2 * i + 1]) == priority;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// ---- generation ----
void BeginChange() {
    g_mutating.fetch_add(1, std::memory_order_seq_cst);
    g_gen.fetch_add(1, std::memory_order_seq_cst);
}
void EndChange() {
    g_gen.fetch_add(1, std::memory_order_seq_cst);
    g_mutating.fetch_sub(1, std::memory_order_seq_cst);
}

// The list moved while the generation did not: something changed it outside the hooked methods
void NoteUnhookedChange(uintptr_t mgr, const char* how) {
    BeginChange();
    EndChange();
    c_unhooked.Add();
    if (!g_unhookedLogged.exchange(true))
        LOG_WARNING(std::format("[ResourceCache] The package list of manager {:#010x} changed without RegisterDatabase / SetDatabasePriority ({}); every remembered answer "
                                "was dropped. Further ones are only counted (Developer page).",
                                mgr, how));
}

// ---- table access (callers hold g_lock) ----
inline uint32_t Hash(uint32_t mgr, const uint32_t* k) {
    uint32_t h = mgr * 0x9E3779B1u;
    h = _rotl(h ^ (k[0] * 0x85EBCA77u), 13);
    h = _rotl(h ^ (k[1] * 0xC2B2AE3Du), 13);
    h = _rotl(h ^ (k[2] * 0x27D4EB2Fu), 13);
    h ^= k[3] * 0x165667B1u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

inline bool SameKey(const Entry& e, uint32_t mgr, const uint32_t* k) {
    return e.mgr == mgr && e.key[0] == k[0] && e.key[1] == k[1] && e.key[2] == k[2] && e.key[3] == k[3];
}

const Snapshot* SnapFor(uintptr_t mgr, uint32_t gen) {
    for (const Snapshot& s : g_snap)
        if (s.gen == gen && s.mgr == mgr) return &s;
    return nullptr;
}

// What a lookup needs, copied out of the table (no lock held afterwards)
struct Answer {
    uint32_t provider = 0;
    int32_t priority = 0;
    uint32_t index = 0;
    uint32_t tick = 0;
    uintptr_t begin = 0;
    uint32_t count = 0;
    int n = 0;                          // non-read-only packages above the answer
    uint32_t prov[kMaxWritableAbove] = {};
};

// The non-read-only packages above `index` in the snapshot, into a (false when they cannot all be listed)
bool WritableAbove(const Snapshot& s, uint32_t index, Answer& a) {
    a.n = 0;
    int i = 0;
    for (; i < s.recorded && s.idx[i] < index; i++) {
        if (a.n >= kMaxWritableAbove) return false;
        a.prov[a.n++] = s.prov[i];
    }
    // packages past the recorded ones may lie above the answer only when every one was recorded
    return i < s.recorded || s.recorded == s.writable;
}

bool Find(uintptr_t mgr, const uint32_t* key, uint32_t gen, Answer& a) {
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    if (g_table && g_stampGen == gen) {
        uint32_t slot = Hash(static_cast<uint32_t>(mgr), key) & kMask;
        for (int d = 0; d < kMaxProbe; d++, slot = (slot + 1) & kMask) {
            const Entry& e = g_table[slot];
            if (e.stamp != g_stamp) break; // empty
            if (!SameKey(e, static_cast<uint32_t>(mgr), key)) continue;
            const Snapshot* s = SnapFor(mgr, gen);
            if (s && WritableAbove(*s, e.index, a)) {
                a.provider = e.provider;
                a.priority = e.priority;
                a.index = e.index;
                a.tick = e.tick;
                a.begin = s->begin;
                a.count = s->count;
                found = true;
            }
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

// The stored answer still is what the game's lookup would return
bool Recheck(uintptr_t mgr, const uint32_t* key, uint32_t gen, const Answer& a) {
    if (GetTickCount() - a.tick > kMaxAgeMs) return false;
    if (!ListEntryIs(mgr, a.begin, a.count, a.index, a.provider, a.priority)) return false;
    if (!Holds(a.provider, key)) return false;
    for (int i = 0; i < a.n; i++)
        if (Holds(a.prov[i], key)) return false;
    return g_mutating.load(std::memory_order_acquire) == 0 && g_gen.load(std::memory_order_acquire) == gen;
}

// Stores the game's answer (found under generation gen, with no list change around it)
void Remember(uintptr_t mgr, const uint32_t* key, uint32_t provider, int32_t priority, uint32_t gen) {
    uint32_t index = 0;
    if (!FindInList(mgr, provider, priority, index)) {
        c_notCached.Add();
        return;
    }
    // The snapshot of this list under gen: build it when missing (outside the lock)
    Snapshot fresh;
    bool haveSnap = false, moved = false;
    AcquireSRWLockShared(&g_lock);
    if (const Snapshot* s = SnapFor(mgr, gen)) {
        haveSnap = true;
        uintptr_t begin = 0;
        uint32_t count = 0;
        moved = !ReadList(mgr, begin, count) || begin != s->begin || count != s->count;
    }
    ReleaseSRWLockShared(&g_lock);
    if (moved) {
        if (g_gen.load() == gen && g_mutating.load() == 0) NoteUnhookedChange(mgr, "list moved or resized");
        c_notCached.Add();
        return;
    }
    if (!haveSnap && !BuildSnapshot(mgr, fresh)) {
        c_notCached.Add();
        return;
    }
    const uint32_t tick = GetTickCount();
    AcquireSRWLockExclusive(&g_lock);
    bool stored = false;
    if (g_gen.load() == gen && g_mutating.load() == 0) {
        if (!haveSnap && !SnapFor(mgr, gen)) { // (another thread may have stored it meanwhile)
            fresh.gen = gen;
            // a free slot (another generation) first, else round robin
            int slot = -1;
            for (int k = 0; k < kSnapshots && slot < 0; k++)
                if (g_snap[k].gen != gen) slot = k;
            if (slot < 0) slot = g_snapNext++ % kSnapshots;
            g_snap[slot] = fresh;
        }
        Answer probe;
        const Snapshot* s = SnapFor(mgr, gen);
        if (s && g_table && WritableAbove(*s, index, probe)) {
            if (g_stampGen != gen) {
                if (static_cast<int32_t>(gen - g_stampGen) > 0) { // a newer generation: the table starts empty
                    g_stamp++;
                    g_stampGen = gen;
                    g_count = 0;
                }
            }
            if (g_stampGen == gen) {
                if (g_count >= kMaxFill) {
                    g_stamp++;
                    g_count = 0;
                    c_fullClears.Add();
                }
                uint32_t slot = Hash(static_cast<uint32_t>(mgr), key) & kMask;
                for (int d = 0; d < kMaxProbe; d++, slot = (slot + 1) & kMask) {
                    Entry& e = g_table[slot];
                    const bool empty = e.stamp != g_stamp;
                    if (!empty && !SameKey(e, static_cast<uint32_t>(mgr), key)) continue;
                    if (empty) g_count++;
                    std::memcpy(e.key, key, sizeof e.key);
                    e.mgr = static_cast<uint32_t>(mgr);
                    e.provider = provider;
                    e.priority = priority;
                    e.index = index;
                    e.tick = tick;
                    e.stamp = g_stamp;
                    stored = true;
                    break;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    (stored ? c_inserted : c_notCached).Add();
}

// Every kFingerprintEvery answers: the whole list still matches the snapshot of this generation
void MaybeFingerprint(uintptr_t mgr, uint32_t gen) {
    static std::atomic<uint32_t> s_hits{0};
    if ((s_hits.fetch_add(1, std::memory_order_relaxed) % kFingerprintEvery) != 0) return;
    c_fingerprintChecks.Add();
    uintptr_t begin = 0;
    uint32_t count = 0, fp = 0;
    if (!Fingerprint(mgr, begin, count, fp)) return;
    bool differs = false;
    AcquireSRWLockShared(&g_lock);
    if (const Snapshot* s = SnapFor(mgr, gen)) differs = s->begin != begin || s->count != count || s->fingerprint != fp;
    ReleaseSRWLockShared(&g_lock);
    if (differs && g_gen.load() == gen && g_mutating.load() == 0) NoteUnhookedChange(mgr, "list content differs");
}

std::string KeyText(const uint32_t* k) { return std::format("{:08X}:{:08X}:{:08X}:{:08X}", k[0], k[1], k[2], k[3]); }

uint32_t VtableOf(uint32_t db) {
    uint32_t vt = 0;
    MemPatch::ReadBytes(db, &vt, 4);
    return vt;
}

// Development build: the game's own lookup for the same key, compared with the cache's answer
bool ShouldVerify() {
    if constexpr (kPublicBuild) return false;
    if (GetTickCount64() < g_verifyAllUntil.load(std::memory_order_relaxed)) return true;
    const int every = g_verifyEvery.load(std::memory_order_relaxed);
    return every > 0 && (++t_verifyCount % static_cast<uint32_t>(every)) == 0;
}

uint32_t Verify(FindFn next, void* self, void* edx, const uint32_t* key, int32_t* priorityOut, const Answer& a, uint32_t gen) {
    uint32_t keyBefore[4];
    std::memcpy(keyBefore, key, sizeof keyBefore);
    int32_t priority = INT32_MIN;
    const int32_t m0 = g_mutating.load();
    const uint32_t r = next(self, edx, key, &priority);
    const bool keyChanged = std::memcmp(keyBefore, key, sizeof keyBefore) != 0;
    const bool settled = m0 == 0 && g_mutating.load() == 0 && g_gen.load() == gen;
    if (r) *priorityOut = priority; // the game's answer is returned in every case below
    if (!settled) {
        c_inconclusive.Add();
        return r;
    }
    if (r == a.provider && (!r || priority == a.priority) && !keyChanged) {
        c_verified.Add();
        return r;
    }
    c_mismatches.Add();
    g_selfDisabled.store(true);
    uint32_t gameIndex = 0xFFFFFFFF;
    if (r) FindInList(reinterpret_cast<uintptr_t>(self), r, priority, gameIndex);
    const std::string text = std::format("key {}: cache said package {:#010x} (vtable {:#010x}, priority {}, index {}), the game says {:#010x} (vtable {:#010x}, priority {}, index {}){}",
                                         KeyText(keyBefore), a.provider, VtableOf(a.provider), a.priority, a.index, r, r ? VtableOf(r) : 0, r ? priority : 0,
                                         static_cast<int32_t>(gameIndex), keyChanged ? "; the game's lookup changed the key in place" : "");
    {
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        g_lastMismatch = text;
    }
    LOG_ERROR("[ResourceCache] Verification mismatch, the cache turned itself off for this session: " + text);
    return r;
}

// ---- hooks (every thread) ----
uint32_t __fastcall Hook_FindProvider(void* self, void* edx, const uint32_t* key, int32_t* priorityOut) {
    const FindFn next = reinterpret_cast<FindFn>(SlotChain::Next(Site::FindProvider, Layer::ResourceCache));
    LookupNote& note = t_note;
    note.seen = true;
    note.hit = false;
    note.probes = 0;
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed) || !key || !priorityOut) return next(self, edx, key, priorityOut);
    c_lookups.Add();
    const uintptr_t mgr = reinterpret_cast<uintptr_t>(self);
    if (g_mutating.load(std::memory_order_acquire) != 0) {
        c_bypassed.Add();
        return next(self, edx, key, priorityOut);
    }
    const uint32_t gen = g_gen.load(std::memory_order_acquire);
    const uint64_t t0 = kPublicBuild ? 0 : Qpc();
    Answer a;
    if (Find(mgr, key, gen, a)) {
        if (Recheck(mgr, key, gen, a)) {
            note.hit = true;
            note.probes = 1 + static_cast<uint32_t>(a.n);
            c_hits.Add();
            c_probes.Add(1 + static_cast<uint32_t>(a.n));
            if constexpr (!kPublicBuild) {
                g_hitTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
                if (ShouldVerify()) return Verify(next, self, edx, key, priorityOut, a, gen); // returns (and writes) the game's answer
            }
            *priorityOut = a.priority; // what the game writes on a find: the package's priority
            MaybeFingerprint(mgr, gen);
            return a.provider;
        }
        c_rejected.Add();
    }
    // The game's own lookup; its answer is stored when no list change happened around it
    c_misses.Add();
    const int32_t m0 = g_mutating.load(std::memory_order_acquire);
    const uint32_t g0 = g_gen.load(std::memory_order_acquire);
    const uint64_t t1 = kPublicBuild ? 0 : Qpc();
    const uint32_t r = next(self, edx, key, priorityOut);
    if constexpr (!kPublicBuild) g_missTicks.fetch_add(Qpc() - t1, std::memory_order_relaxed);
    if (!r) {
        c_notFound.Add();
        return r;
    }
    if (m0 == 0 && g_mutating.load(std::memory_order_acquire) == 0 && g_gen.load(std::memory_order_acquire) == g0) Remember(mgr, key, r, *priorityOut, g0);
    else c_notCached.Add();
    return r;
}

// List changes and change notices: the generation moves before and after, and lookups meanwhile go to the game
uint64_t __fastcall Hook_RegisterDb(void* self, void* edx, uint32_t add, uint32_t db, uint32_t priority) {
    BeginChange();
    const uint64_t r = reinterpret_cast<This3Fn>(SlotChain::Next(Site::RegisterDb, Layer::ResourceCache))(self, edx, add, db, priority);
    EndChange();
    c_listChanges.Add();
    return r;
}
uint64_t __fastcall Hook_RegisterDbDerived(void* self, void* edx, uint32_t add, uint32_t db, uint32_t priority) {
    BeginChange();
    const uint64_t r = reinterpret_cast<This3Fn>(SlotChain::Next(Site::RegisterDbDerived, Layer::ResourceCache))(self, edx, add, db, priority);
    EndChange();
    c_listChanges.Add();
    return r;
}
uint64_t __fastcall Hook_SetDbPriority(void* self, void* edx, uint32_t db, uint32_t priority) {
    BeginChange();
    const uint64_t r = reinterpret_cast<This2Fn>(SlotChain::Next(Site::SetDbPriority, Layer::ResourceCache))(self, edx, db, priority);
    EndChange();
    c_listChanges.Add();
    return r;
}
uint64_t __fastcall Hook_DbChanged(void* self, void* edx, uint32_t db, uint32_t keys) {
    BeginChange(); // its own FindProvider calls (one per changed key) go straight to the game
    const uint64_t r = reinterpret_cast<This2Fn>(SlotChain::Next(Site::DbChanged, Layer::ResourceCache))(self, edx, db, keys);
    EndChange();
    c_notices.Add();
    return r;
}

// ---- the read-only package class, checked on the running build (the "never gains keys" premise) ----
bool MatchBytes(uintptr_t at, const char* pattern) {
    size_t n = 0;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        n++;
        p += 2;
    }
    if (!n) return false;
    uint8_t buf[64];
    if (n > sizeof buf || !MemPatch::ReadBytes(at, buf, n)) return false;
    return MemPatch::ScanPattern(buf, n, pattern) == reinterpret_cast<uintptr_t>(buf);
}

// Target of the first "mov ecx,esi; call X" followed by `after` (2 bytes) within `range` bytes of fn, or 0
uintptr_t CallAfterMovEcxEsi(uintptr_t fn, size_t range, uint8_t after0, uint8_t after1) {
    uint8_t b[0x100];
    if (range > sizeof b || !MemPatch::ReadBytes(fn, b, range)) return 0;
    for (size_t i = 0; i + 9 <= range; i++)
        if (b[i] == 0x8B && b[i + 1] == 0xCE && b[i + 2] == 0xE8 && b[i + 7] == after0 && b[i + 8] == after1) {
            int32_t rel;
            std::memcpy(&rel, b + i + 3, 4);
            return fn + i + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        }
    return 0;
}

bool CheckReadOnlyClass(uintptr_t vtable, std::string& why) {
    uint32_t openRecord = 0, deleteRecord = 0;
    if (!vtable || !MemPatch::ReadBytes(vtable + 0x34, &openRecord, 4) || !MemPatch::ReadBytes(vtable + 0x40, &deleteRecord, 4)) {
        why = "its vtable is not readable";
        return false;
    }
    // OpenRecord 0x007345D0: fast path through the key set when no record / info is asked
    if (!MatchBytes(openRecord, "53 55 8B 6C 24 10 85 ED 56 57 8B 7C 24 14 8B F1 75 ?? 83 7C 24 28 00 75 ?? 6A 01 32 DB E8")) {
        why = std::format("its OpenRecord {:#010x} differs", openRecord);
        return false;
    }
    // ... whose slow path (mov ecx,esi; call; mov bl,al) is the base OpenRecord 0x0072D470, which only opens existing
    // records (dispositions 6 and 3)
    const uintptr_t base = CallAfterMovEcxEsi(openRecord, 0xA0, 0x8A, 0xD8);
    if (!base || !MatchBytes(base, "8B 44 24 10 83 EC 1C 83 F8 06 56 8B F1 74 ?? 83 F8 03 74 ?? 32 C0")) {
        why = std::format("its base OpenRecord {:#010x} can create records", base);
        return false;
    }
    // DeleteRecord 0x007346A0 -> 0x00624F70 "xor al,al; ret 4"
    const uintptr_t del = CallAfterMovEcxEsi(deleteRecord, 0x30, 0x8B, 0x16);
    if (!del || !MatchBytes(del, "32 C0 C2 04 00")) {
        why = std::format("its DeleteRecord {:#010x} can delete records", deleteRecord);
        return false;
    }
    return true;
}

struct Hooked {
    Site site;
    void* hook;
};
const Hooked kWatchers[] = {
    {Site::RegisterDb, reinterpret_cast<void*>(&Hook_RegisterDb)},
    {Site::RegisterDbDerived, reinterpret_cast<void*>(&Hook_RegisterDbDerived)},
    {Site::SetDbPriority, reinterpret_cast<void*>(&Hook_SetDbPriority)},
    {Site::DbChanged, reinterpret_cast<void*>(&Hook_DbChanged)},
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("ResourceCache", &missing)) return fail(GameAddr::NotAvailable(missing));
    std::string why;
    g_readOnlyVtable = GameAddr::Get(GameAddr::Id::ShadowedDbVtable);
    g_readOnlyOk = CheckReadOnlyClass(g_readOnlyVtable, why);
    if (!g_readOnlyOk) return fail("The game's read-only package class was not recognised (" + why + "); nothing could be cached");
    if (!g_table) {
        g_table = static_cast<Entry*>(VirtualAlloc(nullptr, kSlots * sizeof(Entry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); // zeroed: every stamp 0 = empty
        if (!g_table) return fail("Could not allocate the table (2.5 MB)");
    }
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    // Nothing stored before this start is used again
    BeginChange();
    EndChange();
    // The list watchers first, so no change can happen unseen once lookups are answered; then the lookup itself
    int done = 0;
    for (; done < static_cast<int>(std::size(kWatchers)); done++) {
        std::string err;
        if (!SlotChain::Install(kWatchers[done].site, Layer::ResourceCache, kWatchers[done].hook, &err)) {
            for (int k = 0; k < done; k++) SlotChain::Remove(kWatchers[k].site, Layer::ResourceCache);
            return fail("Could not hook the package list: " + err);
        }
    }
    std::string err;
    if (!SlotChain::Install(Site::FindProvider, Layer::ResourceCache, reinterpret_cast<void*>(&Hook_FindProvider), &err)) {
        for (int k = 0; k < done; k++) SlotChain::Remove(kWatchers[k].site, Layer::ResourceCache);
        return fail("Could not hook the resource lookup: " + err);
    }
    g_selfDisabled.store(false);
    g_unhookedLogged.store(false);
    g_on.store(true, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[ResourceCache] On: FindProvider {:#010x} and the package-list methods hooked through their vtable slots; read-only package class {:#010x}; "
                         "table {} entries",
                         SlotChain::GameFunction(Site::FindProvider), g_readOnlyVtable, kSlots));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the layer passes every call through from now on
    SlotChain::Remove(Site::FindProvider, Layer::ResourceCache);
    for (const Hooked& h : kWatchers) SlotChain::Remove(h.site, Layer::ResourceCache);
    BeginChange(); // drop everything stored (a later start begins empty)
    EndChange();
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[ResourceCache] Off ({} lookups, {} answered from the cache, {} list changes, {} change notices, {} unhooked changes, {} mismatches)", s.lookups, s.hits,
                         s.listChanges, s.changeNotices, s.unhookedChanges, s.mismatches));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }

void VerifyAllFor(double seconds) { g_verifyAllUntil.store(GetTickCount64() + static_cast<uint64_t>(seconds * 1000.0)); }

LookupNote TakeLookupNote() {
    LookupNote n = t_note;
    t_note = LookupNote{};
    return n;
}

Stats GetStats() {
    Stats s;
    s.lookups = c_lookups.Get();
    s.hits = c_hits.Get();
    s.misses = c_misses.Get();
    s.bypassed = c_bypassed.Get();
    s.rejected = c_rejected.Get();
    s.notFound = c_notFound.Get();
    s.inserted = c_inserted.Get();
    s.notCached = c_notCached.Get();
    s.probesOnHits = c_probes.Get();
    s.listChanges = c_listChanges.Get();
    s.changeNotices = c_notices.Get();
    s.unhookedChanges = c_unhooked.Get();
    s.fullClears = c_fullClears.Get();
    s.verified = c_verified.Get();
    s.mismatches = c_mismatches.Get();
    s.inconclusive = c_inconclusive.Get();
    s.hitMs = static_cast<double>(g_hitTicks.load()) * g_qpcMs;
    s.missMs = static_cast<double>(g_missTicks.load()) * g_qpcMs;
    s.capacity = kSlots;
    s.readOnlyClassOk = g_readOnlyOk;
    s.selfDisabled = g_selfDisabled.load();
    AcquireSRWLockShared(&g_lock);
    s.entries = g_stampGen == g_gen.load() ? g_count : 0;
    uint32_t newest = 0;
    for (const Snapshot& snap : g_snap)
        if (snap.gen && snap.gen >= newest) {
            newest = snap.gen;
            s.listSize = static_cast<int>(snap.count);
            s.writableProviders = snap.writable;
        }
    ReleaseSRWLockShared(&g_lock);
    {
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        s.lastMismatch = g_lastMismatch;
    }
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off after a check found a different answer (see ApexRadiance_LOG.txt)";
    if (!s.lookups) return "On (no lookups yet)";
    const double pct = 100.0 * static_cast<double>(s.hits) / static_cast<double>(s.lookups);
    const double asked = s.hits ? static_cast<double>(s.probesOnHits) / static_cast<double>(s.hits) : 0.0;
    return std::format("On: {:.0f}% of lookups answered from memory ({:.1f} packages asked instead of up to {})", pct, asked,
                       s.listSize >= 0 ? std::to_string(s.listSize) : std::string("all"));
}

void RenderDeveloperUI() {
    if constexpr (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    // per-second rates from the previous call (render thread only)
    static Stats prev;
    static uint64_t prevTick = 0;
    static double rateHits = 0.0, rateLookups = 0.0, rateSavedMs = 0.0, rateHitMs = 0.0, rateMissMs = 0.0;
    const uint64_t now = GetTickCount64();
    if (now - prevTick >= 1000) {
        const double dt = prevTick ? static_cast<double>(now - prevTick) / 1000.0 : 0.0;
        if (dt > 0.0) {
            const double hits = static_cast<double>(s.hits - prev.hits), lookups = static_cast<double>(s.lookups - prev.lookups);
            const double hitMs = s.hitMs - prev.hitMs, missMs = s.missMs - prev.missMs;
            const double avgMiss = s.misses ? s.missMs / static_cast<double>(s.misses) : 0.0; // long-run average of the game's lookup
            rateHits = hits / dt;
            rateLookups = lookups / dt;
            rateHitMs = hitMs / dt;
            rateMissMs = missMs / dt;
            rateSavedMs = (hits * avgMiss - hitMs) / dt; // estimate: each answer from memory would have cost an average game lookup
        }
        prev = s;
        prevTick = now;
    }
    ImGui::TextUnformatted(("Resource lookup cache: " + StatusText()).c_str());
    ImGui::TextDisabled("Lookups %llu, answered from memory %llu, game lookups %llu (not found %llu), re-check failed %llu, passed through during list changes %llu",
                        static_cast<unsigned long long>(s.lookups), static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
                        static_cast<unsigned long long>(s.notFound), static_cast<unsigned long long>(s.rejected), static_cast<unsigned long long>(s.bypassed));
    ImGui::TextDisabled("Stored %llu (not stored %llu), entries %u / %u, table restarts when full %llu", static_cast<unsigned long long>(s.inserted),
                        static_cast<unsigned long long>(s.notCached), s.entries, s.capacity, static_cast<unsigned long long>(s.fullClears));
    ImGui::TextDisabled("Package list: %d packages, %d not of the read-only class (asked on every answer)%s", s.listSize, s.writableProviders,
                        s.readOnlyClassOk ? "" : "; read-only class NOT recognised");
    ImGui::TextDisabled("Invalidations: list changes %llu, change notices %llu, changes the hooks missed %llu", static_cast<unsigned long long>(s.listChanges),
                        static_cast<unsigned long long>(s.changeNotices), static_cast<unsigned long long>(s.unhookedChanges));
    ImGui::TextDisabled("Per second: %.0f lookups, %.0f from memory; time in answers %.2f ms, in game lookups %.2f ms; saved about %.2f ms", rateLookups, rateHits, rateHitMs,
                        rateMissMs, rateSavedMs);
    ImGui::TextDisabled("Average: game lookup %.1f us, answer from memory %.2f us (%.1f packages asked)", s.misses ? 1000.0 * s.missMs / static_cast<double>(s.misses) : 0.0,
                        s.hits ? 1000.0 * s.hitMs / static_cast<double>(s.hits) : 0.0, s.hits ? static_cast<double>(s.probesOnHits) / static_cast<double>(s.hits) : 0.0);
    int every = g_verifyEvery.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Check 1 answer in N against the game##RcVerify", &every, 0, 1024)) SetVerifyEvery(every);
    ImGui::SameLine();
    if (ImGui::SmallButton("Check every answer for 10 s##RcVerifyAll")) VerifyAllFor(10.0);
    const bool checkingAll = GetTickCount64() < g_verifyAllUntil.load();
    ImGui::TextDisabled("Checks: %llu equal, %llu different, %llu inconclusive (the list changed during the check)%s", static_cast<unsigned long long>(s.verified),
                        static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.inconclusive), checkingAll ? "  [checking every answer]" : "");
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
}

} // namespace ResourceCache
