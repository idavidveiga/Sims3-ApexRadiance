# Faster Create-a-Style — native investigation

**Status: NOT A PLAYABLE PERFORMANCE FEATURE.** A native CASt request-observation prototype is present, but its
embedded Mono x86 InternalCall calling convention is not verified in the game. The switch is off by default and
the code rejects enabling it in a normal build unless the developer explicitly compiles with
`APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS`. Even that flag does not prove the runtime is compatible.

## Verified managed call path

The user's own official `gameplay.package` / `scripts.package` contain:

```text
UI.dll: CASCompositorController.PopulateMaterialsBinGridTimeslicedTask
  -> CompositorUtil.GetPatternThumbnail
  -> SimIFace.ObjectDesigner.GetPatternThumbnail
  -> IWorld.ObjectDesigner_GetPatternThumbnail (native InternalCall)
```

The game's CASt task walks the pattern list and yields between items, but builds a fresh
`MaterialsGridItem` layout for each item. This can make scrolling or first-time loading slow.

## Current Apex prototype

In developer builds only, `features/fast_create_a_style.cpp` intercepts the native ICall resolution
and records repeat pattern-thumbnail request signatures (compositor ID, pattern hash, input-array length
and input hash). It **always invokes the original game's native function**. It **does not reuse
thumbnails**, copy output buffers, or return cached native handles.

The old experimental implementation incorrectly read `MonoArray.max_length` at x86 offset `0x08`,
which corresponds to the array-bounds pointer. The corrected offset is `0x0C`, and the array
vector begins at `0x10`, consistent with the documented 32-bit Mono object layout. Method name/
namespace strings are now copied into bounded local buffers under protected reads.

Most importantly, the native function returns a handle whose ownership and validity were never
established. Returning a cached handle could draw the wrong thumbnail. That replay path has been
removed until a reliable lifetime/invalidation strategy exists.

The observations are bounded to 2,048 unique request signatures. No game DLLs are replaced.

## Remaining work

- Verify the installed `TS3W.exe` or `TS3.exe` Mono ABI and resolver address for that game build.
- Establish the native thumbnail handle lifecycle or identify a safe, independent decoded-image cache.
- Implement stable-slot/visible-first population of the actual CASt grid; the current native ICall hook
  alone **cannot** reschedule the managed loop.
- Build x86 `ApexRadiance.asi`, test with CC patterns, compare original/revised image correctness, and measure timings.

The feature must not be released as a performance optimization until those tasks pass.
