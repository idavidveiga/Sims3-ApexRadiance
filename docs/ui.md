# Menu UI (Violet design)

The in-game menu of Apex Radiance (window id `###ApexWindow`, default key Ctrl+Shift+F11; layout saved in
`Documents\...\Apex Radiance\apex_radiance_imgui.ini`). Visible names come from `apex_version.h` (`APEX_PRODUCT_NAME` =
"Apex Radiance", `APEX_PRODUCT_TAGLINE` = "for The Sims 3", `APEX_LOGO_LETTER`); internal names keep "Apex". Write
"Apex Radiance" in visible text through `APEX_PRODUCT_NAME`, never the old "S3SS Apex" / "Apex Edition".

History: reorganised on 2026-09-28 for players (short plain words, one feature per card, developer items on one
Developer page); the same day it got a design polish (spacing scale, row descriptions, dividers, tinted notes, button
styles), a full copy rewrite, and the grouped sidebar with tabbed pages below. Later that day (approved from a mockup):
search, changed markers with per-setting Reset, the undo toast, Profiles, peek, hold to compare, GPU cost
chips, "Reload save" badges, inline "Turn on" dependencies, the welcome tour, the status bar, the collapsible sidebar,
colour tracks and keyboard use (sections below).

## Files
- `apex_gui.cpp`: window, header, sidebar, pages, search results, Profiles, welcome tour, undo toast, status
  bar, first-launch hint. Header: the logo (ui/logo.h: ui/apex_logo.png as a 128x128 texture with mips, ui/logo_data.h; the old violet tile with `APEX_LOGO_LETTER` only if the texture cannot be made), name and tagline (centred on the logo), the search field, Night/Day pill
  (moon / sun), frame-time pill, close (x); all vertically centred on the 32 px tile (narrow windows drop the Night/Day
  pill, then the frame-time pill, to keep the search field at least 110 px). Sidebar 170 px at scale 1 (or the 44 px
  icon rail) with group labels and, at the bottom, the collapse button and the version; the page in a scrolling child;
  the status bar under both. Default window 560 x 640 at scale 1 (min 400 x 300).
- `ui/widgets.{h,cpp}` (`ApexUi`): cards, rows and the other components, see "Components".
- `ui/icons.{h,cpp}` + `ui/lucide_data.h`: the icons, see "Icons".
- `ui/violet_theme.{h,cpp}` (`VioletTheme`): palette, the global style and the fonts. Apex owns its ImGui context, so
  `Overlay::Init` applies both once. Sizes are at 1080p; the overlay scales the style by resolution
  (`0.9 * pow(h / 1080, 0.8)`) and the user's text size and sets `style.FontScaleMain`. Widget geometry uses
  `ApexUi::Unit()` (= `FontScaleMain`): never write a pixel size without `* u`.
- Fonts: Segoe UI 15 px (+ Bold for titles). Missing files: ImGui's default font. No icon font.
- Feature code draws its own controls: `EdgeSmoothingPatch` / `DepthBlurPatch::RenderCustomUI` (card body) and
  `RenderDeveloperUI` (Developer page), `Picture::RenderUI(tab)` / `RenderDeveloperUI`, `Borderless::RenderUI`, and
  Night Lights through `patches/night_lighting.h` (one function per card).

## Pages
Sidebar: Overview, then three groups (small upper-case muted labels): WORLD, IMAGE, SYSTEM. Pages with tabs use the
underline `TabBar` under the page title; the selected page and each page's tab are statics (kept while the game runs,
not saved; `Go(page, &tab, n)` opens a page on a tab).

| Page (sidebar icon) | Content |
|---|---|
| Overview (layout-dashboard) | Sims3SettingsSetter recommendation card (only while S3SS is not loaded and `[ui] recommend_s3ss` is true; "Download", "Don't show again"). One card listing every feature as a row (icon, name = link to its page and tab, phrase, GPU cost chip, switch; Borderless shows its mode in a pill): Night Lights, Water Reflections ("Needs Night Lights" / "Needs Depth Blur" when one is off), Picture, Depth Blur, Borderless, Edge Smoothing, Faster File Lookups (gauge; `ResourceLookupCache`), Faster Room Lighting (gauge; `RoomLightQueue`, since 2026-09-29), Lot Lighting While Moving (gauge; `LotLightingMotion`); the three open the Performance page. |
| WORLD > Lighting (moon-star) | Tabs **Lamps** (Night Lights card: master switch `NightTerrainRelight` + "Lamp color" Pink ... Warm white with a colour track, swatch and "Reload save" badge; then "Reset Night Lights"), **Ground** (Ground & Lots: street lamps light lots, lot lamps light the street, smooth ground light, BRIGHTNESS), **Objects** (every option shown, groups LAMP LIGHT and DOORS, COUNTERS AND FENCES; "Light stairs, railings, columns" has the "Reload save" badge; the DOORS group needs two Ground options: a note naming the missing one(s) and a primary "Turn it on" / "Turn both on"), **Buildings** (Buildings card: groups WALLS and ROOFS; Rooms at Night card (moon, since 2026-09-29): "Darker unlit rooms" + Light left, Blue tint, Soft light on furniture), **Stories** (since 2026-09-29, the user asked for everything about stories in its own tab; Stories card (layers): "Upper floors light the ground" = the separate `SplitLevelGroundLight` feature, shown on and disabled with "Already handled by Sims3SettingsSetter ..." when S3SS's own fix is on; "Outdoor light between floors"; "Seamless walls between floors" and "Indoor light between floors" (Experimental), both disabled with a "Needs ..." note while "Outdoor light between floors" is off). While Night Lights is off, the other tabs show a note and a primary "Turn on Night Lights" button. |
| WORLD > Water & Snow (waves-horizontal) | Tabs **Water** (Lamp Glow card while Night Lights is on, else "Lamp glow on ponds needs Night Lights" + "Turn on Night Lights"; Water Reflections card = `reflexoNoLago`, with "Needs Night Lights (Lighting page)" / "Needs Depth Blur (Depth Blur page)" and primary "Turn on ..." buttons) and **Snow** (walked-on sidewalks; needs Night Lights; needs "Street lamps light lots": note + "Turn it on"). |
| IMAGE > Color (palette) | First the Banding Fix card (blend icon; switch only, on by default; a note on what it covers; features/banding-fix.md). Then the Picture card header above the tabs (switch = `[qol.picture] enabled`; GPU cost chip; hold to compare (eye) and before / after (columns-2) buttons, disabled while Picture is off, never saved). Tabs **Basic** (brightness, contrast, saturation, temperature, sharpness, smooth gradients), **Tones** (midtones, shadows, highlights, blacks), **Color** (tint, vibrance; FILM TONES = split toning, hue sliders on a hue-circle track with a swatch; COLOR MIXER + "Reset mixer"), **Detail** (clarity, vignette, vignette size); each tab ends with "Reset Picture". Rows stay visible, greyed out, while Picture is off. |
| IMAGE > Ambient Occlusion (contrast) | Note: turn off the game's own Edge Smoothing; performance note (gauge) "Heavier on the graphics card than other effects: lower the Quality if the game slows down". Ambient Occlusion card (GPU cost chip): Strength (0-200%, 100% = the recommended look), Quality Very Low / Low / Medium / High / Ultra (2 / 4 / 6 / 8 / 12 slices); Advanced: Reach (50-200%), Keep lamp light (0-100%), "Show the shade alone" (not saved); "Reset Ambient Occlusion". Developer > Debug views: status, GPU cost, camera read-out, "Show the shade alone". See features/ambient-occlusion.md. |
| IMAGE > Depth Blur (aperture) | Note: turn off the game's own Edge Smoothing. Depth Blur card (GPU cost chip): Focus Auto / Fixed (segmented), Blur amount (%); Auto: Sharp area Small / Medium / Large; Fixed: Distance Near / Medium / Far + "Fine-tune distance" (0-100% of 0..0.5) + Transition; Sharp in map view; Advanced (rare knobs): Strength, Quality, Focus speed (Auto only, "0.3 s"), Blur the sky, Glowing lights; "Reset Depth Blur". Mode-specific rows are drawn (and searchable) only in their mode. |
| SYSTEM > Display (monitor) | Tabs **Window** (Borderless card: Window mode Normal / Borderless window / Borderless fullscreen (maximize) as a segmented row, a list when it does not fit; restart / info note; "Handled by Sims3SettingsSetter" when S3SS owns it) and **Anti-aliasing** (note: turn off the game's own Edge Smoothing; Edge Smoothing card: Method SMAA (recommended) / FXAA, Quality steps per method, FXAA-only Softness / Sensitivity shown with FXAA; "Reset Edge Smoothing"). |
| SYSTEM > Performance (gauge; added 2026-09-29) | One card "Performance" ("Fewer stutters while you play"; `PerformanceCard`, no header switch; [features/performance.md](features/performance.md)): switch rows, each drawn by `FeatureSwitchRow` (the feature description on hover of the row, "Not available" / "Starting…" / error notes under it), in this order: "Faster game file lookups" ("Fewer small stutters when objects and textures load"; `ResourceLookupCache`, default off; while it is on, the row "Remember missing files" ("Skips repeated searches for files no package has"; `ResourceLookupMisses`, default off) under it), "Faster file lists" ("Fewer stutters when Sims load outfits and shapes"; `FileListCache`, default off), "Spread lot lighting while moving" ("Lots relight in small steps while the camera moves"; `LotLightingMotion`, default on; while it is on, the slider "Lot lighting time while moving" (1-15, value "3 ms", end labels "Smoother" / "Lights sooner", default 3; `Performance::SetLotLightingBudgetMs`, saved as `budgetWhileMovingMs`)), "Wall shading waits while moving" ("Walls of new lots get their shading when you stop"; `WallShadingWhileMoving`, default on), "Faster texture compression" ("Fewer hitches when the game builds terrain, Sim and lot textures"; `FastTextureCompression`, default off; while it is on, the row "Use several cores" ("Large textures are shared out over several processor cores, with the same result"; `useSeveralCores`, default on) under it), "Faster cache compression" ("Fewer hitches when the game stores Sims and objects in its caches"; `FastCacheCompression`, default off), "Spread new objects over frames" ("Fewer hitches when a lot streams in while the camera moves"; `SceneNodeBudget`, experimental, default off) and "Faster object lookups" ("Fewer hitches when lot lights update; less script work"; `ObjectLookupIndex`, experimental, default off). Only the lot lighting row has a slider; the other tuning is in the Developer card. |
| SYSTEM > Developer (wrench; development build only) | ImGui tabs: Lighting (Night Lights status, census (list-checks), diagnostics (stethoscope), light probe (crosshair), counters, the generic list of every option; Every-Story Ground Light state), Profiler (activity; Frame Profiler, the Apex shaders line, then the "Performance" dev card: resource lookup cache counters (with the "Remember missing files" and file list cache lines), "Check 1 answer in N against the game", "Check every answer for 10 s", lot lighting call / camera / budget lines, the wall shading gate lines with the slider "Longest wait of a pass while moving (ms)" (0-10000, default 2000), the texture / cache compression lines, the scene node budget lines with the sliders "Nodes per frame while moving" (8-4096, default 512), "ms per frame while moving" (0.1-10, default 2.0) and "Longest wait (ms)" (16-5000, default 500), and the object lookup index lines with "Check 1 answer in N against the game" (default 64) and "Check every answer for 10 s"; none of these is saved), Capture (camera; Frame Capture), Debug views (bug; Edge Smoothing status / GPU cost / red pixels, Depth Blur status / focus mode and depth / GPU cost / "Show blur amount" / far plane, Picture's 8-bit note and GPU cost). |
| SYSTEM > Settings (settings) | Tabs **Menu** (control rows: menu key + Change, text size - 100% + Reset steps, "Show the welcome tour again" + Show, Save now), **Profiles** (see "Profiles"), **Compatibility** (rows Game, Sims3SettingsSetter Installed (circle-check) / Not installed, Features; the recommendation + Download while S3SS is missing; "Details" = the raw S3SS summary and settings migration note), **About** (name, version and build in the header; CREDITS). |

Every card is `PushID(<name>)` + `BeginCard("##Card")` ... `EndCard()` + `PopID()`. Every setting is a row drawn by
`SwitchRow` / `Slider` / `SliderPercent` / `SegmentedRow` whose label is its stable id (unique within its card, `##`
suffix when two labels read the same, e.g. `Brightness##Walls`); keep it that way: search, changed markers and undo
reports all hang off these rows. Each page tab's content is its own function in `apex_gui.cpp` (`LampsTabContent`,
`GroundTabContent`, ..., `PictureHeaderCard`, `PictureRows(tab)`, `DepthBlurContent`, `BorderlessCard`,
`AntiAliasingContent`, `PerformanceCard`, `MenuTab`) so the page and the search results draw the same code.

## Design tokens
All sizes are 1080p pixels at text size 1, times `ApexUi::Unit()`.

**Palette** (`violet_theme.h`): accent #7F77DD, accent dark #534AB7, accent light #CECBF6, window #15161A, card #1C1D22,
border / dividers #2A2B31, selected #24252B, hover #1F2025, switch off #3A3B42, text #E8E8EC, muted #8B8C96, warning
(amber) #E0A84F, error (soft red) #E8716B, success (muted green, status bar only) #7DBE9A. Disabled = style alpha 0.5
(`DisabledAlpha`) on the whole row. Keyboard focus ring = ImGui's nav cursor in accent light (#CECBF6), 2 px, rounded
like the frame, on every nav-focusable widget (all custom widgets use `InvisibleButton` with
`ImGuiButtonFlags_EnableNav`, so ImGui draws it).

**Chips** (`Chip`, 0.8x text, 6 px side padding, 14% fill of their colour): muted for GPU cost ("~0.4 ms"), amber for
"Reload save". **Changed dot**: 3 px radius, accent, 6 px after the label (after the badge when there is one).

**Spacing scale** (`kSpace1..4` = 4 / 8 / 12 / 16):
- window padding 12; content area padding 8 x 4; sidebar hairline, then 12 to the page;
- card padding 16 x 12, card rounding 10, 12 between cards (`EndCard`), 12 under the page title / tab bar;
- `CardDivider()` between a card's header and its body: 12 above the hairline, 12 below; only when a body follows;
- rows: consecutive rows are `kRowGap` (16) apart, label to label, with a hairline in the middle; anything that is not a
  row (note, button, "Advanced", group label) keeps 8 from a row above it (12 for a group label);
- label to description 2; description / label to a slider track or segmented control 5 (item spacing).

**Typography**: page title bold 1.3x, page subtitle muted 1x; card title bold 1.05x, card subtitle muted 1x; row
label 1x `kText`; description 0.87x muted; slider value 1x muted, right-aligned on the label's line; end labels, pills
and the sidebar version 0.87x muted; group labels ("WALLS", sidebar "WORLD") bold 0.8x muted, upper case, letter-spaced.

**Icons**: on card headers (18, accent), the sidebar (18), overview rows (18), notes (14), buttons (14), pills (14).
Setting rows have no icons (all rows in a card look the same).

## Components (`ApexUi`, ui/widgets.h)
- `PageTitle(title, subtitle)`; `TabBar(id, &tab, labels, n, icons)`: underline tabs (selected = white text + 2 px
  accent underline, others muted, hairline under the bar, wraps when narrow).
- `BeginCard` / `EndCard` (always call EndCard), `CardHeader(icon, title, subtitle, tooltip, toggle, toggleEnabled,
  extra)` (`HeaderExtra`: an icon toggle left of the switch, used by before / after), `CardDivider()`.
- Rows (dividers between consecutive rows): `SwitchRow(label, v, description, default)` (switch right, centred on the
  text block); `Slider(label, v, min, max, SliderOptions)` (label + value on one line, description, track; options:
  format / scale / offset, fixed value text, end labels, colour swatch, `defaultValue`, a gradient track
  `trackFrom`/`trackTo` or the hue circle `hueTrack`) and `SliderPercent(label, v, min, max, description, default)`;
  `SegmentedRow(label, description, id, current, labels, count, tooltips, icons, defaultIndex)`;
  `BeginControlRow(label, description, controlsWidth)` (returns false when the search hides it: then draw nothing and
  skip `EndControlRow`) / `EndControlRow()` for custom controls on the right (menu key, text size, info rows, profiles);
  `OverviewRow(..., chip)`. The description is always visible (no hover tooltip on rows); `SliderCommitted()` =
  deactivated after edit of the last slider, or its Reset (Picture saves then). `SetNextRowBadge(text, tooltip)` puts a
  chip after the next row's label.
- Defaults are optional arguments (`BoolDefault` from a bool, `kNoDefault` / `kNoDefaultIndex` = none); pass them
  from the code's real defaults (registered setting defaults, `PictureParams{}`, the `Params{}` of Edge Smoothing and
  Depth Blur, `Borderless::Mode::Off`, `ApexPatch::IsEnabledByDefault()`), never guessed.
- `CardHeader(..., extra, chip)`: `HeaderExtra` also has a hold button (`holdIcon`, `holdTooltip`, `held` while
  pressed); `chip` = GPU cost. Header switches, overview switches and rows report their changes (`ReportChange`).
- `Segmented(id, current, labels, count, tooltips, compact, icons)`: segments share the width (a vertical list when they
  do not fit); selected = accent dark fill with accent light text; hover tooltips per segment; `current = -1` = none
  selected (a fine-tuned Depth Blur distance).
- `BeginAdvanced` / `EndAdvanced`: hairline, then an accent chevron + "Advanced" row (full-width click target),
  collapsed by default (state per id in the card's storage); contents are not indented. Use it only for rare knobs (a
  page or tab with room shows options directly). `AdvancedNode` (tree node) stays for the Frame Profiler.
- `GroupLabel("WALLS")`: groups inside a card.
- `IconNote(icon, text, rgb)`: icon + wrapped text in a tinted rounded box (10% of its colour): info = `Info`, muted;
  warning = `TriangleAlert`, `kWarning`; error = `TriangleAlert`, `kError`.
- Buttons, one frame high: `IconTextButton(label, icon, tooltip, ButtonKind)` and `TextButton(label, tooltip, kind,
  minWidth)`; `ButtonKind::Secondary` (neutral fill, border turns accent on hover: Reset, Save, Change, - / +) and
  `ButtonKind::Primary` (accent dark fill, accent on hover, white text: "Turn on ...", "Download"). `ButtonWidth` for
  control rows. "Don't show again" stays a text link.
- `IconButton`, `Pill`, `Chip`, `SidebarItem(icon, label, selected, collapsed)` (32 high; collapsed = icon only, label
  as tooltip), `SidebarGroup(text, collapsed)` (collapsed = a short hairline), `ToggleSwitch`, `Tooltip`, `MutedText`,
  `Gap`.
- Search filter: `BeginFilter(query)`, `SetFilterCrumb(crumb, id)`, `EndFilter(&clicked)`, `FilterActive()`; change
  reports: `ReportChange`, `SetChangeReporting`, `TakeChange`; drag fade: `SliderDragging`, `SetKeepActiveSliderOpaque`.

## Features of the menu (how they work, where their state lives)

State: everything below lives in `apex_gui.cpp` statics (render thread, inside the overlay's ImGui frame) unless a
`[ui]` key is named. Persisted `[ui]` keys go through `ApexConfig::GetUi` / `SetUi` (saved by the pump thread a second
later): `welcome_done` (default false; missing = false, so migrated configs see the tour once) and `sidebar_collapsed`
(default false). Feature state changes go through the features' own paths (`ApexPatch`, `NotifySettingChanged`,
`PatchManager` unsaved flag, `Picture::SetParams`, `Borderless::SetMode`), so autosave works as before.

### Search
Header field "Search settings" (search icon, "Ctrl+F" hint while empty; Esc or the x clears it). Ctrl+F focuses it,
only while the menu window has keyboard focus (then ImGui captures the keyboard, so the game never sees it). While the
query is not empty the content shows **Search**: one card with every matching row, live (the real controls). Matching:
every word of the query (case-insensitive, ASCII) must appear in the row's visible label or its description. How: the
page tabs are functions; `SearchResults` calls each one (`SearchParts`, in sidebar order: Lighting tabs, Water & Snow
tabs, Color header + Color tabs, Depth Blur, Display tabs, Performance, Settings > Menu) between `ApexUi::BeginFilter` and
`EndFilter`. In filter mode the row widgets draw only when they match, each after a small muted breadcrumb link
("Lighting › Lamps"); card frames, dividers, notes, buttons, group labels, page titles, tab bars and "Advanced"
headers draw nothing (buttons inside a visible control row still draw); a card header with a switch becomes a
searchable switch row (so "night lights" or "depth blur" finds the master switches). Feature card bodies and Night
Lights tabs are searched even while the feature is off. Clicking a breadcrumb opens that page and tab and clears the
search; so does picking a sidebar page. Not searched: Overview (it repeats the master switches), Developer, Profiles,
Compatibility, About.

### Changed markers and per-setting Reset
A row with a default shows a violet dot after its label while its value differs from the default ("Changed from the
default" on hover); hovering the row shows a small Reset button (rotate-ccw) right after it, which puts that one setting
back through the row's normal change path (the widget sets the value and returns "changed", so Night Lights'
`ApplyLive`, Picture's save, etc. run as for a click). Sliders compare with a tolerance of 1/10000 of their range.
The small Reset buttons are not keyboard stops (they only exist on hover).

### Undo toast
At the start of each frame, when the left mouse button goes down inside the menu window or Space / Enter / keypad Enter
is pressed while it has focus (and no slider was active in the previous frame, so the key that ends a keyboard
adjustment does not count), the menu takes `ApexConfig::CaptureFeatureState` (every `[patches.*]` table, `[qol.picture]`,
`[display]`). Rows, card-header switches and overview switches report what changed (`"<Label> turned on/off"`,
`"<Label> changed"`, `"<Label> reset"`; sliders once, on release); Reset buttons and "Turn on" buttons report too. The
last report of a frame becomes the toast at the bottom right of the window, above the status bar: the text and an
"Undo" link (undo-2), about 4 s, fading in and out; the timer waits while the pointer is on it. Undo =
`ApexConfig::ApplyFeatureState(snapshot)`: only sections that differ are applied (feature settings and on / off through
`ApexPatch::ApplyTableLive`, Picture through `SetParams`, the window mode through `Borderless::SetMode`), then unsaved +
save. Only the last change is undoable. The Developer page does not report (`SetChangeReporting(false)`); menu
preferences (text size, menu key, sidebar) are not part of the undoable state.

### Looks (removed)
A "Looks" card (Classic / Balanced / Cinematic presets on Overview and in the tour) existed briefly and was removed on
2026-09-28 at the user's request: applying a look rewrote the player's tuned Night Lights, Picture and Depth Blur values
(only the last change is undoable), which destroyed a hand-tuned setup. Do not bring presets back without the user asking;
if ever, save the current setup as a profile first.

### Profiles (Settings > Profiles)
"SAVE CURRENT SETUP": "What to save" = a checkbox per part (Night Lights, Color, Depth Blur, Edge Smoothing,
Window mode, Performance, Shortcuts (off by default), Ambient Occlusion (bit 7, added 30/09 so older part masks keep their bits); the rest checked by default, kept while the game runs), then a name field (only letters, digits, space, - and _ can be typed; at most 32 characters; Enter
saves) and Save; an existing name asks "... already exists; replace it?" inline. "SAVED": one row per profile with Load
and Delete; the row's description lists the parts the file has. Load opens an inline pick of those parts (all
checked) with Load / Cancel, and applies only the checked ones (`KeepProfileParts`). Delete asks inline, "Delete this
profile?" with Delete / Cancel. "Open the Profiles folder" opens it in Explorer (to share profiles or copy them to
another PC). Profile names are user data: shown untranslated (`SetNextRowUntranslated`). Files:
`Documents\...\Apex Radiance\Profiles\<name>.toml`, written by `ApexConfig::SaveProfile` = `CaptureFeatureState(profile
features only)` + `[meta]`: the same tables the main config saves for Night Lights (`NightTerrainRelight`), Every-Story
Ground Light, Edge Smoothing, Depth Blur and the Performance page's features (`[patches.<Name>]` with `enabled`), Picture (`[qol.picture]`) and the
window mode (`[display]`); developer tools stay out. Load = `ReadProfile` + `ApplyFeatureState` (live, marks unsaved
changes, autosaves into ApexRadiance.toml) + the undo toast "Profile loaded". Names are sanitised
(`SanitizeProfileName`: allowed characters only, no leading / double / trailing spaces, Windows device names refused);
files whose names do not survive it are not listed. The list is read when the tab opens and after each action.

### Peek and the drag fade
Holding Alt while the pointer is over the menu makes it nearly transparent (window 0.2, contents 0.1 through the
disabled alpha) and inert (`BeginDisabled`), so the game shows through; releasing restores it (eased over about 0.1 s).
Not while typing or dragging. While a slider is dragged (or adjusted with the keyboard) the window fades to 0.35 and the
active slider's row stays fully opaque (`SetKeepActiveSliderOpaque`). The status bar says "Hold Alt to peek". Keys:
while the menu has keyboard focus ImGui captures the keyboard (the existing policy), so Alt never reaches the game;
while the pointer is only hovering the menu, `GuiClient::CaptureKey` claims Alt and B (not while typing): the overlay
eats their key-down, key-up and character after ImGui saw them (`framework/overlay.cpp`; cleared on focus loss). Alt+Tab
is handled by Windows before any window sees it, so it keeps working.

### Hold to compare (Color page)
The eye button in the Picture header (left of before / after), or holding B while the pointer is over the menu (not
while typing), calls `Picture::HoldBypass()` every frame: the Picture pass is skipped for 0.15 s after the last call
(`m_holdUntil`, never saved), so the game shows its original picture and it comes back by itself on release.

### GPU cost chips
The Overview rows and card headers of Picture, Edge Smoothing and Depth Blur show "~0.4 ms" (tooltip "Measured cost on
your GPU per frame") from the features' own timestamp queries: `Picture::GpuMs()` and `ApexPatch::GpuCostMs()`
(Edge Smoothing and Depth Blur override it with their measured pass time). Hidden while the feature is off or not
measured yet.

### "Reload save" badges
An amber chip after the label of rows whose change shows only after a save or world loads again, tooltip "This change
shows after you load a save again": Lamp color (lamps are tinted when their colour is written, i.e. when a save loads)
and "Light stairs, railings, columns" (those pieces get their light when a world loads). Their descriptions no longer say
it, and the old footer "Some changes show after you reload your save" is gone. Every other Night Lights option applies
live (docs/features/night-lighting/README.md, "Applied" column).

### Inline dependencies
A row or card whose effect needs another feature or option shows an info note naming it (and where it is) plus a primary
button that turns it on directly (and reports the change, so it can be undone): Night Lights tabs ("Turn on Night
Lights"), Lamp Glow ("Turn on Night Lights"), Water Reflections ("Turn on Night Lights" / "Turn on Depth Blur"), Objects >
DOORS, COUNTERS AND FENCES ("Turn it on" / "Turn both on" for "Street lamps light lots" and "Smooth ground light"), Snow
("Turn it on" for "Street lamps light lots"). The game's own Edge Smoothing (Depth Blur, Edge Smoothing) is a game
option, so it stays a plain note.

### Welcome tour and the first-launch hint
The first time the menu opens in a session while `[ui] welcome_done` is false, the content area shows the tour (a card
with step dots; the sidebar and the search field are disabled meanwhile): 1) "Sims3SettingsSetter" ("Installed; you're all set", or the recommendation + Download), 2) "Your menu key" (the key +
Change). Buttons: Skip (link, step 1), Back, Next, Done; Skip and Done set `welcome_done = true`. Settings > Menu >
"Show the welcome tour again". The menu is never opened automatically: instead, at every launch (30/09: it used to be only
while the tour was never done), 2 s after the features run (on the game's first loading screen, the user's wish of 30/09), a
small non-blocking note (no input, no focus) shows in the top-left corner: the user's pick "A · compact pill" (logo,
**Apex Radiance is ready** in bold, a dot, "press" and the menu key in light violet; dark pill #15161a at 92%, border
#CECBF6 at 18%). It stays 8 s of time on screen (each frame counts at most 100 ms, so a loading stall does not use it up)
or until the menu is opened, and fades out over the last 0.8 s; `Client::AlwaysDraw` keeps ImGui frames going meanwhile.
Not shown while the first-start key choice is pending.

**Shortcuts and the first-start prompt (30/09).** Every Apex shortcut is eaten by the overlay's window procedure before
the game sees it (the menu chord as before; the others through `Client::HotkeyDown` -> `Hotkeys::OnKeyDown`), so no game
key can clash. They come in presets (`hotkeys.h`, `[ui] hotkey_preset`; missing = the F keys, as earlier versions):

| | Letters (recommended) | Numbers | F keys |
|---|---|---|---|
| Menu (`toggle_key`) | Ctrl+Shift+R | Ctrl+Shift+1 | Ctrl+Shift+F11 |
| Compare with the game | Ctrl+Shift+T | Ctrl+Shift+2 | Ctrl+Shift+F10 |
| Refresh the lighting | Ctrl+Shift+G | Ctrl+Shift+3 | Ctrl+Shift+F9 |
| Dev: Light Probe / Light Diag / recorder / Frame Capture | V / B / X / F | 4 / 5 / 6 / 7 | F7 / F8 / F6 / F5 |

Letters: left-hand keys next to each other, in the same place on QWERTY, ABNT2, AZERTY and QWERTZ, no Fn (A Q W Z move
on some layouts; C is the cheat console; D S E move the camera; M is the map). Numbers: easiest to remember. F keys: the
earlier keys. Compare turns Night Lighting, Depth Blur, Edge Smoothing and the picture filters off and back (not saved;
a note shows at the top while off). Refresh does what the Developer buttons "Rebuild terrain light now" and "Relight lots
now" do plus every room and the object rigs (NightLighting::RefreshAll). `compare_key` / `refresh_key` hold the player's
own keys (none yet in the menu: reserved).
While `[ui] key_chosen` is false (missing = false: every existing config sees it once), a centred window shows the three
presets with their keys (Letters marked recommended) and "Customize…" (an own menu key, the rest on Letters). A choice
sets the preset, the menu key and `key_chosen`, closes the window; pressing the
current menu key closes it keeping that key (F keys preset). Settings > Menu: "Shortcuts" (the preset) and the keys of
the quick actions, then "Menu key" (Change: any key).

### Status bar
A thin footer under the sidebar and page (hairline, then one line of small text): left "All changes saved" (circle-check,
muted green) or "Saving…" (save icon) while `ApexConfig::SavePending()` (a requested save or unsaved feature changes);
middle "Sims3SettingsSetter detected" (violet check) / "Sims3SettingsSetter not installed" (dropped when narrow); right
"Hold Alt to peek". It replaces the header's unsaved dot.

### Collapsible sidebar
The chevrons button at the bottom of the sidebar collapses it to a 44 px icon rail (items show their name as a tooltip,
group labels become short hairlines, the version hides) and expands it again; saved in `[ui] sidebar_collapsed`.

### Colour tracks
Lamp color: a gradient track from the game's pink to warm white and a swatch of the current colour, both computed from
the real tint math (`LampColourAt` in `night_terrain_relight_patch.cpp`, the same blend as
`ObjectLightBridge::TintStockColour`: stock pink (1, 0.75, 0.79) to warm white (1, 0.80, 0.62) scaled to the pink's
luminance; shown as they are, an approximation of the on-screen colour). Picture's Shadow color / Highlight color: a
hue-circle track and a swatch of the hue.

### Keyboard
`ImGuiConfigFlags_NavEnableKeyboard` is on (overlay). Arrows / Tab move between widgets; Space / Enter toggles switches
and presses buttons; a slider is adjusted with the arrows after Space / Enter. Esc (when the menu has focus, no field is
being edited and the menu key is not being chosen): clears the search, else closes the
menu. Ctrl+F: the search field.

## Languages

English, Portuguese (Brazil), Spanish and French (Settings > Menu > Language; "Automatic" follows Windows' display
language; saved as `[ui] language = "auto" / "en" / "pt" / "es" / "fr"`). `ui/i18n.h`: the English text is the key,
translations are in `i18n/tr_widgets.cpp` (texts the widgets write themselves), `tr_menu.cpp` (apex_gui.cpp),
`tr_image.cpp` (Color, Edge Smoothing, Depth Blur, Borderless), `tr_lighting.cpp` (Night Lights, Water & Snow) and
`tr_features.cpp` (performance features, framework notices). Every widget translates what it draws; ImGui IDs stay the
English label, so switching languages keeps the menu's state. The search matches both the English and the shown text.
A text without a translation shows in English; the development build lists them in Developer > Language (also
translations whose `{}` placeholders differ from the English). How to add texts: `i18n/TRANSLATING.md`. Logs and the
Developer page stay English.

## Copy guidelines
- American English, friendly and plain, for players: say what the player will **see**, not how it works. No jargon
  outside the Developer page (no "per-pixel", "lightmap", "shader", "depth", "bridge", "rig", "story").
- Vocabulary: **lamps** (not lights / lamp light mixed), **lots**, **street**, **ground**, **brightness** (for
  strengths, shown in %), **upper floors** (not stories). "Night Lights" is the feature name everywhere; "Lighting" is
  only its page.
- Sentence case for labels, tabs and buttons ("Street lamps light lots"); Title Case only for page and card titles
  ("Ground & Lots", "Lamp Glow").
- Labels: at most ~32 characters, no final period, positive (a switch says what turning it on does: "Sharp in map
  view", "Outdoor light between floors", not "Off in ..." / "... aren't cut").
- Descriptions, notes, button and segment tooltips: one short sentence, ideally at most 60 characters (never more than
  ~90), **no final period**; join two ideas with a semicolon ("How bright lit roofs get; 60% is the default").
- Defaults are written "100% is the default" / "0% is off" / "100% is unchanged".
- Values: % or named steps (Near / Medium / Far, Low ... Ultra) instead of raw numbers; signed amounts as "+25".
- Notes that point elsewhere name the place: "(Ground tab)", "(Lighting page)", "(Depth Blur page)", "(Options ›
  Graphics)".
- Prose blocks keep full sentences with periods: feature hover descriptions (they end with "Part of Apex Radiance.
  Credits: @loinyx"), the Sims3SettingsSetter recommendation, the startup banners and the credits.

## Icons (ui/icons.h)
Lucide icons (ISC License, Copyright (c) Lucide Icons and Contributors; the license text is
`third_party/lucide/LICENSE`, the source SVGs are `third_party/lucide/icons/*.svg`). Credited in Settings > About >
Credits ("Lucide icons (ISC)").
- `ui/lucide_data.h`: each used icon's SVG elements copied as data (path `d` strings, circle cx/cy/r, rect
  x/y/width/height/rx, line x1/y1/x2/y2, and `filled` for `fill="currentColor"` dots). No build step: to add an icon,
  copy its elements from the .svg by hand, append it to `kIcons` and to `ApexUi::IconId` in the same order (a
  static_assert checks the count). Ten icons at the end of the list (search, undo-2, chevron-left, chevrons-left,
  chevrons-right, check, gauge, eye, bookmark, trash-2) are not in `third_party/lucide/icons/`: their elements were
  entered by hand from Lucide's published icons; if one looks off, download its .svg and replace the data.
- `ui/icons.cpp`: on first use an icon's paths are parsed (M L H V C S Q T A Z, absolute and relative; arcs by the SVG
  endpoint-to-centre conversion) and flattened to polylines in the 24x24 viewBox, then cached. Drawing scales them:
  `ImDrawList::AddPolyline` with thickness 2/24 of the size (Lucide's stroke-width 2, at least 1 px), closed for Z; round
  caps and joins as small filled circles at open ends and at corners sharper than ~25 degrees; circles with `AddCircle`,
  rects with `AddRect` and their corner radius; filled dots with `AddCircleFilled`. No texture, no device dependency,
  render thread only.
- API: `DrawIcon(dl, id, pos, size, color)`, `Icon(id, pos, size, color)` (current window), `InlineIcon(id, size, color)`
  (reserves the square with a Dummy), `IconLabel(id, label, color)`. Sizes: `kIconSmall` 14 and `kIconMedium` 18, times
  `Unit()`.

## Rules kept from the previous menu
- Feature switches are inert while features start ("Starting…") or on an unsupported game ("Not available on ...");
  `GetLastError()` shows as an error note; a feature's own controls appear only while it is on (Picture's stay visible,
  greyed; the search results show them either way).
- The description (ending with "Credits: @loinyx") is the hover tooltip of the card title, the card switch and the
  overview name; no visible credit lines besides the Credits section.
- Developer items only under `if constexpr (!kPublicBuild)` (the Developer page is not in the public sidebar).

## Startup banners (drawn even while the menu is closed)
- Old combined build loaded (`Startup::RefusedOldBuild`): "Apex Radiance is off" (error colour), the module name, and
  "Delete that file from Game\Bin, keep the official Sims3SettingsSetter.asi, then restart the game." Features stay off.
- Older standalone `S3SSApex.asi` loaded too (`ApexGui::SetOldStandaloneNotice`): "An older S3SSApex.asi is also
  installed. Delete it from Game\Bin." (warning colour). Features keep running (the old copy idles). When the old copy
  loaded first, Apex Radiance itself idles (no menu, no banner) and only writes an error line to `ApexRadiance_LOG.txt`.

## Credits (Settings > About)
sims3fiend (Sims3SettingsSetter, the model for the rewritten framework), FXAA 3.11 (Timothy Lottes), third-party code
(Dear ImGui, Microsoft Detours, toml++, SMAA, Lucide icons), the single line "Every-Story Ground Light uses a technique
first shared by Arro.", and "Apex Radiance by @loinyx". Do not mention Arro or the Split-Level fix anywhere else
(feature descriptions, tooltips, release notes, promo text); functional notices about official S3SS's own fix being on
("Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)") stay.
- Brand note: removed 30/09 (the faint name in the bottom-right corner was drawn with the terrain, often still under the load screen, and the user did not see it); the "is ready" note above now shows at every start instead.
- Recommendations (30/09): DXVK (detected by the loaded d3d9.dll: not in the Windows folder and "dxvk" inside the file)
  and official Sims3SettingsSetter. Only the missing ones are listed, each with Download (GitHub releases): at every start
  a note in the top-left corner (before the shortcuts note; "Not now" = this session, "Don't show again" = `[ui]
  recommend_s3ss = false`, now for both), the Overview card "Recommended for Apex Radiance", and Settings > Compatibility
  (DXVK and Sims3SettingsSetter rows with Installed / Not installed, the RECOMMENDED group while one is missing).
