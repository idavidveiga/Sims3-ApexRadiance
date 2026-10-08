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

## Follow-up live snapshot (00:38:40 local)

The player exported `ApexRadiance_LOG_LIVE(1).txt` without closing the
game, using the new manual log snapshot action. The repeated full scan
reported **12,441,402/12,441,402 bytes read, 0 skipped, exactly one match
at RVA 0x00A826A0** and recorded the candidate neighborhood:

```text
preceding: 04 28 00 5D C3 CC CC CC CC CC CC CC CC CC CC CC
signature: 81 EC 08 08 00 00 53 55 8B AC 24 14 08
next:      00 00 85 ED 56 57 75 1C 68 BC 16 10 01 68 D7 1A 00 00
```

The contiguous `CC` bytes immediately preceding the signature are
consistent with alignment/fill between x86 functions, but are not proof
of the target function's identity.

Under the assumption that RVA 0x00A826A0 is a genuine function entry,
`sub esp, 0x808; push ebx; push ebp; mov ebp, [esp+0x814]`
loads the value at **entry ESP+4**, consistent with a pointer passed
as the first stack argument in 32-bit x86. This does **not** tell us
whether the callee or caller cleans the arguments, identify Mono
method structures, or establish that hooking is safe.

The snapshot contains no logged ERROR/CRITICAL events, but has seven
WARN entries, mostly slow overlay frames and one late Present notice.
Apex CAS and CASt hooks were not installed by this diagnostic.

## Next evidence needed

Capture a longer **read-only** region after the candidate prologue to
inspect branch/call patterns and eventual return conventions. Keep
function identity and calling convention explicitly unverified until
corroborated; do not activate any hooks. The existing test only logs a
short neighborhood and cannot establish how the function returns.

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
