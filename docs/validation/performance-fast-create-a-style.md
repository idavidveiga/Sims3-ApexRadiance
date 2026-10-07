# Faster Create-a-Style: validation

The feature is described in [features/performance/fast-create-a-style.md](../features/performance/fast-create-a-style.md).

## In-game test plan

1. Start with the feature on and open Build/Buy Create-a-Style for an object with many patterns.
   **Expected:** the game behaves normally and `ApexRadiance_LOG.txt` records that
   `IWorld::ObjectDesigner_GetPatternThumbnail` was resolved.
2. Scroll to the end of a large pattern category, back to the start, then down again.
   **Expected:** no wrong thumbnails, no blank cells, no pattern from another category; cache-hit count rises on repeated
   requests if the stock UI asks for them again.
3. Close CASt and reopen it on the same object/category.
   **Expected:** repeated native thumbnail requests can be answered from the cache; selection and recolouring remain
   correct.
4. Change a pattern/preset in a way that invokes the large-pattern thumbnail create/clear path.
   **Expected:** the old cached preview is not reused after the change.
5. Turn *Faster Create-a-Style* off while CASt is open, browse and scroll, then turn it on again.
   **Expected:** off = game-native behavior; no crash; on again continues safely.
6. Repeat with CC patterns, Store patterns, custom presets and every stock pattern category.
   **Expected:** thumbnails and selected materials match the feature-off result exactly.
7. Compare a cold first opening and a second/repeated opening with the feature on/off.
   Record frame times or the Apex Frame Profiler around the scroll. The cache stage should primarily improve repeated
   requests; it is not expected to remove all first-load cost.

## Log checks

Useful lines:
- `[FastCreateAStyle] Started...`
- `[FastCreateAStyle] IWorld::ObjectDesigner_GetPatternThumbnail -> ...; cache wrapper installed`
- the stop summary with calls / hits / misses / stores / bytes reused.

A build where `mono_lookup_internal_call` cannot be validated must report the feature unavailable rather than patching
an unverified address.

## Pass criteria

- No thumbnail differs from the feature-off result.
- No stale thumbnail survives a create/clear invalidation.
- Off/on switching is safe after the InternalCall has already been resolved.
- Repeated browsing produces cache hits on a real CASt workload.
- If real workloads produce essentially no duplicate native thumbnail requests, do not keep this cache as the final
  optimization; proceed to the managed grid scheduling/visible-row stage instead.
