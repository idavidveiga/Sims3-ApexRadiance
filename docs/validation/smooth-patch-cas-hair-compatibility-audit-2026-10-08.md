# Smooth Patch 2.1 vs Apex CAS Hair/Hats: verified scope and limits

Date: 2026-10-08. Read-only source and behavioral documentation audit.
**Outcome: No verified Smooth Patch 2.x Hair/Hats scheduler to reuse.
Do NOT ship a Hair/Hats modification on the basis of this comparison.**

## Primary sources checked

1. LazyDuchess official ModTheSims release:
   https://modthesims.info/d/658759/smooth-patch-2-1.html
   - 2.0: "Improve CAS clothing loading times."
   - "CAS clothes are now loaded in as you scroll rather than staggered".
   - "The faster clothing loading will disable itself when entering Master
     Controller CAS as the mods are not compatible with each other."
   - 2.0 hotfix 1 addressed null-reference errors in body hair and
     advanced pet coat categories.
   - The page marks Smooth Patch 2.1 unsupported as of 2026-01-26.
2. The author's public **native** source, historically named
   TS3FrameratePatch:
   https://github.com/LazyDuchess/TS3FrameratePatch/blob/master/TS3FrameratePatch/dllmain.cpp
   - The code scans for the common SleepEx wrapper bytes
     `8B 44 24 04 8B 08 6A 01 51 FF` and modifies its first
     instructions to apply TPS/system/uncapped behavior.
   - It hooks D3D9 presentation for an FPS cap and can apply borderless.
   - It does **not** contain `CASHair`, `ItemGrid`, or the 2.x
     managed scrolling optimization.
   - This public source is an older native implementation, **not** a
     source listing of every function in the latest 2.1 ASI.
3. Author's earlier MasterController integration note:
   https://www.patreon.com/lazyduchess/posts/ts3-smooth-patch-72964777
   - Mentions specific regressions (hairstyles disappearing, blacklisting,
     clothing selection, multiple accessories) and known unfinished
     compact-mode support. Compatibility reports are not proof of
     native Hair/Hats scheduling.
4. A dated reply in MTS's release discussion:
   https://modthesims.info/download.php?p=5935575
   - Describes removal of the separate MasterController package and
     disabling of the incompatible clothing list acceleration in MC CAS.
5. Logged managed stack traces in MTS's discussion:
   https://modthesims.info/download.php?c=1&p=5991180
   - Method explicitly identified:
     `LazyDuchess.SmoothPatch.ClothingPerformance.
     DresserOnSimOutfitCategoryChanged(OutfitCategories)`
     fired by `SimOutfitCategoryChanged`, and `CASLogic`.
   - A later issue also shows this method with a NullReferenceException
     after changing outfit categories.
   - These identify a clothing change handler, **not** an actual
     Hair/Hats `PopulateTypesGrid(bool)` replacement.
6. The user's original EA 1.69 UI.dll audit, already validated and
   documented in this repository:
   `docs/research/cas-hair-regression-2026-10-08.md`;
   `tools/verify_hair_ui_contract.py`.
   - `CASHair.PopulateTypesGrid(bool)` = `0x06001918`, 1621 bytes IL,
     calls `AddHairTypeGridItem` twice for default/preset paths.
   - Store items and hair-part groups have distinct enumeration/finally
     regions. Child `AddHairTypeGridItem` uses layout, filters,
     thumbnail creation and `ItemGrid.AddItem` synchronously.
   - The earlier MonoPatcher experiment replacing this child method
     caused all Hair/Hats to vanish; removing the experiment restored
     them. It must not be repeated.
   - Existing clothing `ItemGrid.BeginPopulating/OnPopulateTick`
     does not establish that Hair/Hats uses the same mechanism.

## Technical verdict

| Mechanism | Verified effect | Direct Hair/Hats reuse? |
| --- | --- | --- |
| Native Smooth Patch `.asi` (published old source) | Changes common sleep/TPS behavior, FPS limiting | **No.** Broad timing changes are not managed-grid scheduling and overlap S3SS sites |
| 2.x `ld_SmoothPatch.package` (author's release) | Clothes populate on scroll | **Not proven.** The announced change covers clothes, not explicit Hair/Hats parent loop |
| Smooth Patch + MasterController | CAS clothes acceleration disabled in MC CAS due to incompatibility; earlier hair/catalog regressions reported | **No.** Requires explicit conflict guard and vanilla fallback |
| Original The Sims 3 `ItemGrid` | Clothes already have task-based `BeginPopulating/OnPopulateTick` methods | **Not plug-and-play.** Hair/Hats uses a different synchronous parent with enumerator/finally/preset state |

## Scope of this verification / what was not accessible

The current MTS download includes `ld_SmoothPatch.package` in
`ld_TS3Patch.zip`. The original managed package binary was **not**
available in the user's accessible files, and the site did not permit
retrieving that archive in this session. Consequently the 2.x managed
IL has **not** been extracted, decompiled, diffed or hash-checked;
do not claim that a particular Smooth Patch managed method directly
patches `CASHair`, nor claim it definitely does *not*.
The direct evidence covers public native source, author statements,
release history and reported managed stack traces.

## Independent Apex-native path: only after bridge proof

- Recognize exact original method/assembly (independent
  `features/ts3_mono_method_identity.h` guard already exists).
- Inspect the real active `UI.dll` method *and* MC CAS replacement,
  and refuse integration on unverifiable method ownership.
- Validate whether a separate CAS task can submit **whole hair-part
  groups** to `ItemGrid` while retaining both original enumerator
  finally regions, Store-first order and all thumbnail/preset/selection
  side effects. This has NOT been demonstrated.
- If the original game's `ItemGrid` API cannot preserve those
  constraints, **do not borrow the clothing path**; keep the original
  Hair/Hats parent and optimize proven native metadata/thumbnail
  bottlenecks separately. That alternative does not pretend to
  make the parent loop incremental.
- Do not stack native TPS / SleepEx hooks in the standalone Apex.
- Do not install MonoPatcher, S3MM or an edited EA UI.dll; release
  target remains one standalone native ApexRadiance.asi.

**Go/no-go:** current result is NO-GO for live Hair/Hats time-slicing.
A source-based Clothes-on-scroll feature is not equivalent to a
working Hair/Hats method adapter. No user testing should be requested
until a code-level integration actually exists and passes offline ABI,
lifecycle and regression checks.
