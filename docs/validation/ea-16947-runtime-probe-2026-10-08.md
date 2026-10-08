# EA App 1.69.47 — runtime probe result (2026-10-08)

Source: user-supplied ApexRadiance_LOG(1).txt from an ordinary game launch with the
Win32 diagnostic ApexRadiance.asi. This report contains technical findings only;
it does not include the player's private Windows path or full game log.

## Observations

- ApexRadiance.asi loaded into TS3.exe 1.69.47.024017, Developer mode enabled.
- 64 KiB initial loaded .text sample: entropy 6.472 bits/byte, protection 0x20
  (PAGE_EXECUTE_READ); historical Steam resolver signature absent there.
- Full incremental .text scan started 00:03:37.737 and ended 00:03:40.937.
- Read 12,441,402 of 12,441,402 bytes, skipped 0.
- **One exact historical Steam-shaped signature at RVA 0x00A826A0.**
- Log states explicitly that **ABI and JIT method are UNVERIFIED**.
- No [ERROR] or logged exception in the provided file; four [WARN] lines concern
  delayed initial Present or slow overlays/frames.
- The game also lists Sims3Performance.asi beside Apex. Compatibility checks will
  be required before any new CAS hook is enabled.

## Interpretation

The on-disk EA executable was previously observed to contain high-entropy .text
and no occurrence of the same historical Steam byte signature. The ordinary
loaded game exposes readable .text with one matching byte sequence. This proves
only that the exact bytes appear at the indicated position **at runtime**.

It does **not** identify mono_lookup_internal_call, confirm that RVA
0x00A826A0 begins a function, prove the x86 calling convention, identify the
compiled CAS managed methods or authorize installing a detour. No CAS or CASt
performance gains have been demonstrated or measured.

## Next validation

The next read-only probe logs a bounded byte neighborhood around each candidate
as part of the existing incremental scan, at most eight candidates and without
any extra memory access. It is implemented in
features/ts3_mono_runtime_probe.cpp. A compiled Win32 diagnostic must be run
in the ordinary game to collect that context before any function-identity work.

Keep CAS/CASt native hooks blocked in ordinary builds and keep the logical CAS
scheduler disconnected from the live game until ABI/ownership is validated.
No external method patcher or third-party patching code is allowed.
