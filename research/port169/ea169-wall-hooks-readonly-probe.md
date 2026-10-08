# EA App 1.69.47: wall base and UV hook mapping

This note documents a **read-only process-memory capture** taken on 2026-10-08 from a running TS3.exe.

- Game: EA App 1.69.47.024017
- PE timestamp: `0x6707155C`
- Machine: `0x014C` (x86)
- Module base during capture: `0x00400000`
- Capture regions: `0x006A7000..0x006B1000` and `0x00C30000..0x00C43000`
- The capture was not modified and is not committed, because it contains game code.

## Exact locations (Steam 1.67 -> EA 1.69.47)

| Instruction | Steam | EA 1.69.47 | Expected original bytes |
| --- | ---: | ---: | --- |
| Outdoor wall conditional sequence start | `0x006AB328` | `0x006AC4D8` | `84 C0 74 07 F3 0F 10 45 10 EB 12` |
| Outdoor wall `movss xmm0,[ebp+10h]` patch site | `0x006AB32C` | `0x006AC4DC` | `F3 0F 10 45 10` |
| Indoor wall `movss xmm0,[eax+78h]` patch site | `0x006AB340` | `0x006AC4F0` | `F3 0F 10 40 78` |
| Wall geometry UV writer | `0x00C38530` | `0x00C369B0` | EA prologue `55 8B EC 83 E4 F0 81 EC A8 00 00 00` |
| UV writer call #1 | `0x00C387B5` | `0x00C36C35` | `E8 76 FD FF FF` |
| UV writer call #2 | `0x00C387CC` | `0x00C36C4C` | `E8 5F FD FF FF` |

Every signature was found **exactly once in the relevant read-only capture region**. Both UV CALL relative displacements resolve to `0x00C369B0` on EA 1.69.47. Their original displacement bytes match the Steam signatures even though the absolute target differs.

## Safety gates

The patch to `features/level_light_share.cpp`:
- Restricts this new map to `GameVersion::EA` with timestamp `0x6707155C`, loaded at `0x00400000`.
- Retains the original Steam addresses and guards.
- Validates the original wall instruction bytes, the EA UV-writer prologue, both original UV CALL instructions and their actual original destination **before patching**.
- Assigns the correct per-build UV writer for calls from `WallUvHook`; previously it was hardcoded to the Steam address.
- Restores any partially applied UV call patches if one fails to install.
- Does **not** add support for EA 1.69.43 or unknown builds.

## Remaining work

- Compile the branch for Windows x86 (MSVC), and check the results.
- In-game runtime validation is still required. In `ApexRadiance_LOG.txt`, verify that “Outside walls on foundations,” “Indoor faces of diagonal walls on foundations,” and “Walls taller than a story” report `ready` rather than `not available`.
- Compare screenshots of the **same building and same save/time**, near and far, with the feature on/off. Check walls on foundations, multi-story walls and upper levels. The observed near/distant difference is not conclusively explained solely by these hooks; also examine lighting LOD/lot streaming if it persists.
- The Steam-only wall-floor occlusion and doors/windows cutout hooks remain outside this change.

This is an *experimental* x86 game-code compatibility fix, not a verified visual resolution or release build.
