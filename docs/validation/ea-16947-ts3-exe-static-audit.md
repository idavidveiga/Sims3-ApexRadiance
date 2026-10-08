# EA App TS3.exe 1.69.47 — read-only executable audit

The supplied executable was inspected as raw PE32 bytes. It was **not run, modified, decrypted, or redistributed**.

| Property | Observed |
|---|---|
| Size | 14,887,256 bytes |
| SHA-256 | 7352dd6e599f4475f812bbca19bbc7e5d8e84bbd5b6d21acaac9b1c5236adfe4 |
| Format | PE32 Intel i386 |
| PE timestamp | 0x6707155C = 2024-10-09 23:44:28 UTC |
| Apex known version | EA App 1.69.47.024017 |
| Image base | 0x00400000 |
| Entry point | RVA 0x00EB4000 in .ooa |
| .text | 12,443,648 raw bytes, entropy 8.0000 bits/byte |
| .data | entropy 7.9995 bits/byte |
| .rdata | entropy 5.7214 bits/byte |
| Activation import | Core/Activation.dll |
| Historical Steam Mono resolver byte signature in disk .text | 0 occurrences |

The combination of .ooa entry point, activation import, and very high code entropy indicates an activation-protected static image. **No native Mono resolver or JIT function address can be reliably derived from the file on disk.** An original method address must not be invented from a Steam version.

## Reproduce using your local executable

    python tools/inspect_ts3_executable.py C:\Path\To\TS3.exe --json

The analyzer reads only local bytes, reports section entropy and build data, and never writes into the executable.

## Next step inside the normally running game

Apex now contains read-only runtime diagnostics in:

- features/ts3_mono_runtime_probe.cpp
- features/ts3_mono_runtime_probe.h
- patches/performance_patches.cpp (CAS developer UI)

Once compiled and loaded normally, open Apex Developer → Performance and choose **Inspect loaded TS3.exe (read-only)**. It samples the loaded .text section and checks whether the historical Steam resolver prologue appears. The result is logged using Apex's existing logger. It never installs a hook, writes to game memory, or modifies the executable.

**A signature match is not enough to verify a method address or calling convention.** More native ABI research and an in-game compatibility build are still needed.

## Boundaries

- The two experimental CAS/CASt hooks remain off by default and are blocked in ordinary builds without a developer-only compile flag.
- The new viewport-first C++ scheduler is unit-testable but **not connected to UI.dll**.
- No Win32 Apex ASI build or in-game performance test has been completed.
- No external method patcher or additional managed script package is part of this work.
