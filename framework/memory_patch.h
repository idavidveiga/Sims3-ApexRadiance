#pragma once
// Game-code patching helpers: guarded reads, byte-checked writes with an undo list, relative branch offsets, pattern
// scans, Detours batches and addresses resolved per game build.
#include <windows.h>
#include <cstdint>
#include <detours/detours.h>
#include <optional>
#include <vector>
#include "game_version.h"

namespace MemPatch {

// One write, with the bytes it replaced (restored by RestoreAll).
struct PatchLocation {
    uintptr_t address = 0;
    std::vector<BYTE> original;
};

// Reads count bytes; false (out unchanged) when the memory is not readable.
bool ReadBytes(uintptr_t address, void* out, size_t count);
// True when count readable bytes at address equal expected.
bool ValidateBytes(LPCVOID address, const BYTE* expected, size_t count);

// Writes bytes at address (code or data; protection is lifted for the write and the instruction cache flushed).
// expected: when given, the current bytes must start with it or nothing is written. undo: receives what was there.
bool WriteBytes(uintptr_t address, const std::vector<BYTE>& bytes, std::vector<PatchLocation>* undo = nullptr, const std::vector<BYTE>* expected = nullptr);
bool WriteDWORD(uintptr_t address, DWORD value, std::vector<PatchLocation>* undo = nullptr, const DWORD* expected = nullptr);
// Puts back every recorded write, newest first. On success the list is cleared; on failure it keeps what could not be
// restored.
bool RestoreAll(std::vector<PatchLocation>& undo);
// Writes `count` (1..16) bytes of code while every other thread of the process is suspended and none of them is stopped
// inside (address, address + count) (retried for up to about 100 ms). For rewriting a CALL that several threads may run:
// no thread can fetch half of it. Nothing is allocated and no lock is taken while the threads are suspended. False when
// the write did not happen (nothing changed).
bool WriteCodeSuspended(uintptr_t address, const BYTE* bytes, size_t count);
// Several code writes done together while every other thread of the process is suspended and none is stopped inside
// (address, address + guard) of any of them (retried for up to about 100 ms). guard >= count when no thread may resume
// inside the bytes; smaller when the new and old code share instruction boundaries past `guard`. No check of the current
// bytes and no undo: the caller reads them first. False when a write failed (the earlier ones of the batch stay written).
struct CodeWrite {
    uintptr_t address;
    const BYTE* bytes;
    size_t count;
    size_t guard;
};
bool WriteCodeBatchSuspended(const CodeWrite* writes, size_t n);

// rel32 of a JMP/CALL at `from` (instruction of `length` bytes) that lands on `to`.
int32_t CalculateRelativeOffset(uintptr_t from, uintptr_t to, size_t length = 5);

// Base and size of a loaded module's image.
bool GetModuleInfo(HMODULE module, BYTE** base, size_t* size);
// First match of an IDA-style pattern ("8B 4E ?? E8") in [base, base + size), or 0. Unreadable memory is skipped.
uintptr_t ScanPattern(const BYTE* base, size_t size, const char* pattern);

} // namespace MemPatch

// A batch of Detours inline hooks attached or detached in one transaction (all or nothing).
namespace DetourBatch {
struct Hook {
    void** target; // in: the function to hook; after install: the trampoline to call the original
    void* detour;
};
bool InstallHooks(const std::vector<Hook>& hooks);
bool RemoveHooks(const std::vector<Hook>& hooks);
} // namespace DetourBatch

// A game address: the verified address per build, else a pattern scan of TS3W.exe; either way checked against
// expectedBytes before it is returned.
struct GameAddress {
    struct PerVersion {
        GameVersion version;
        uintptr_t address;
    };
    const char* name = "";
    std::vector<PerVersion> addresses;
    const char* pattern = nullptr;
    int patternOffset = 0;
    std::vector<uint8_t> expectedBytes;

    std::optional<uintptr_t> Resolve() const;
};
