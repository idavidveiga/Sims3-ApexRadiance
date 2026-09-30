#pragma once
// Faster record checksums (part of Faster Cache Compression, feature "FastCacheCompression"; docs/features/performance.md,
// "How it works: record checksums").
//
// The texture compositor's cache package (Sim and material textures; the filter is set up at 0x005BBB8B / 0x005BBF6B and
// by the factory at 0x0072C602) runs every record it writes and reads back through a checksum filter (vtable 0x00FE2594:
// verify 0x0072C580, write 0x0072C610) that calls FUN_004fa4c0: an MSB-first table CRC-32, cdecl(bytes, length, crc, bool invert), one table
// lookup per byte (table 0x0114D330). The loading RE of 30/09 found it in the compositor's cache hitches (6-10% of a
// 490 ms eviction cascade). This answers that function (the entry chain, layer FastCrc) with the same CRC computed eight
// bytes per step ("slicing by 8"): tables derived from the game's own table, so the value is the same for every input.
// Checks: before hooking, the game's table must be a CRC table (T[0] = 0 and T[a ^ b] = T[a] ^ T[b]) and the game's own
// function must give the same values as Apex's on test buffers; the first 16 calls of each session (then, in the development
// build, 1 in 64) also run the game's function and compare: a difference is logged, the game's value is used and the
// part turns itself off for the session.
#include <cstdint>
#include <string>

namespace FastCrc {

bool Start(std::string* error);
void Stop();
bool Running();

struct Stats {
    uint64_t calls = 0, bytes = 0;
    uint64_t checked = 0, mismatches = 0;
    bool selfDisabled = false;
};
Stats GetStats();
std::string StatusText();

} // namespace FastCrc
