# Faster Scripts: validation

The feature is described in [features/performance/faster-scripts.md](../features/performance/faster-scripts.md). It must:

- Give every patched compare handler the same registers, x87 stack and flags as the original, and keep the bytes from the
  old return address on.
- Answer `mono_type_get_object` only with an answer the game gave for the same domain and type while that domain lives.
- Turn the type cache off on the first difference, and restore every patched byte and hook on Stop.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-05 | `a1de9ab` | Offline scan of Steam TS3W.exe | 25 compare and branch handlers matched (0x00E54D1B..0x00E58304); no branch targets the inside of a block | None |

## In-game test plan

1. Start a world. **Expected log:** `[ScriptMath] 25 interpreter compare handlers test NaN inline (...)` and the type
   cache line.
2. Play an hour with an active household. **Expected:** no `[ScriptMath] A stored type object differs ...` error; at
   exit the summary shows answers from the cache and checks, all equal.
3. EA app build. **Expected:** the same lines.

## Confirmed in game

- Nothing recorded at publication.

## Open checks

- The effect on simulation time (Frame Profiler, simulation thread).
- The EA app build in game.
