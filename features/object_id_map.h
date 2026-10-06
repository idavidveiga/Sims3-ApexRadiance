#pragma once
// Object ID side index (Apex Radiance; part of "Faster object lookups", feature "ObjectLookupIndex").
//
// The game's object service ("Objects/Service", [0x011DC350] on Steam 1.67.2, created by 0x0093A510) keeps every object
// by its 64-bit ID in a hash map embedded at service +0x10: 1033 (0x409) buckets of singly linked nodes (the objects
// themselves: id at +8 / +0xC, next at +0x10), keyed by idLo % 1033, the end sentinel at map +0x1024, the count at
// +0x1028. With tens of thousands of objects every chain holds dozens of them, and each lookup walks one (a cache miss
// per node). Its functions (05/10):
//   0x00939100 find, thiscall(map; out[2], key*), ret 8: out = {node, &bucket} or {*(map+0x1024), map+0x1024} (end).
//     Six callers (get, get + addref, destroy, unused ID, batch, check), all object service methods.
//   0x00939170 insert, thiscall(map; out[3], node, flag), ret 0xC: the only writer of new links (callers 0x00939857 and
//     0x0093A8C7); out byte 8 = 1 when the node was linked, 0 when the ID was already there.
//   0x00938D00 erase, thiscall(map; out[2], node, bucket*), ret 0xC: the only unlinker (caller 0x009394A4).
//   Nothing else writes the map's count (+0x1028: only these two and the constructor, scan of every displacement in
//   0x00900000..0x00960000) or hashes with 1033 buckets (the other users of the constant, 0x006FDFB0 / 0x006FE1F0, are a
//   renderer map of their own). Insert and erase are leaves (no calls).
// This index mirrors the map in an open-addressing table (id -> node). Insert and erase run under an exclusive SRW lock
// and update the index with the game's own result; find answers from the index under a shared lock (busy = the game's
// own walk) with exactly the game's output, and only while the index's count equals the map's own count (+0x1028):
// a mutation the hooks did not see turns the index off for the session. The first 256 answers (then 1 in 64 in the
// development build) are also compared with the game's walk.
#include <cstdint>
#include <string>

namespace ObjectIdMap {

bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();

} // namespace ObjectIdMap
