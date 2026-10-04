> Published 2.5.6: System > Performance begins with Optimize rendering, default on
> for missing settings, preserving explicit saved off choices. Its row reset and
> Reset all restore on. One unified ASI offers optional developer mode. The System
> display page is Edge Smoothing only; window and pacing controls are removed.

> Local development (not released): new configurations start with Ambient Occlusion enabled at 168% Strength, 351 m
> Distance and High quality. Advanced defaults are 130% Reach and 38% Keep lamp light. The separate Sim Occlusion
> card starts off; when enabled, its defaults are 47% body intensity, 38% hair intensity and 47% maximum darkening.
> See [Ambient Occlusion](features/ambient-occlusion.md) for shader coverage, fallback behavior, revision migration
> and validation limits.

> Local development: new configurations enable the filtered screenshot shortcut by default on C. Previously saved
> screenshot keys remain unchanged. It
> leaves bare F10 available for the game's UI toggle, which is used only internally while hiding the interface for a
> screenshot; the prior UI state is restored afterwards. Ctrl+Shift+F10 remains Compare.

> Current local development: the Optimize rendering card and its mode switch are
> removed; the existing performance patch controls remain. The published 2.5.6
> behavior described above is historical.

> Published since 2.5.5: Report uses the restored session/capture/list/help layout
> with optional title/description after saving. Earlier guided stages and required
> descriptions are superseded. See [bug-reports.md](features/bug-reports.md).

> Historical RC, superseded before 2.5.5: Display and fluency was tested with
> window/monitor selection and read-only VRR reports, then removed. See
> [display-fluency.md](features/display-fluency.md) for historical findings only.

# Menu UI (Violet design)

## Private RC Overview refinement (2026-10-02)

Overview keeps all eleven existing controls and page/tab links. Three Violet cards group them into Lighting
(Night Lights, Water Reflections, Faster Room Lighting, Lot Lighting While Moving), Image (Picture, Ambient
Occlusion, Banding Fix, Depth Blur, Edge Smoothing), and Performance and screen (Faster File Lookups, Borderless).
Profiles and favorites are not added to Overview. Switches alone convey enabled/disabled state; no repeated
"On" labels. Enabled Edge Smoothing shows its actual method/quality; Depth Blur shows its chosen focus mode.
The existing open-menu backbuffer MSAA check supplies "Waiting for game settings" on affected enabled rows.
Unknown/device-loading state does not invent a conflict. GPU chips retain measured-only timing and hide while
disabled or blocked. Long translated names wrap before the switch/chip.

The discreet reset icon beside the title confirms inline, restores only the displayed enable switches, water
reflection amount and window mode, and retains fine tuning, language, shortcuts, developer preferences and saved
files. Existing toast Undo is reused. Whole-mod reset is kept in Settings, not Overview. All new strings are EN,
PT-BR, ES and FR. No new hooks, timers, render passes, background work or saved keys. Compilation and translation
checks do not replace in-game verification of spacing, loading/Reset compatibility and restoration/Undo.

Private update, 2026-10-02: on-screen startup, recording, capture and comparison notices now share the same top-center position and Violet pill style. See [bug-reports.md](features/bug-reports.md#one-click-selection-and-centered-notices-private-2026-10-02) for priority, click confirmation and return-to-Report behavior. The simpler recording/saving layout is now implemented locally as `2.5.4-test-report-library`, with required details replacing the capture workspace rather than opening a modal. Panel opening and notices wait for the loaded world; this is not part of the published 2.5.4 binary.

The in-game menu of Apex Radiance (window id `###ApexWindow`, default key Ctrl+Shift+F11; layout saved in
`Documents\...\Apex Radiance\apex_radiance_imgui.ini`). Visible names come from `apex_version.h` (`APEX_PRODUCT_NAME` =
"Apex Radiance", `APEX_PRODUCT_TAGLINE` = "for The Sims 3", `APEX_LOGO_LETTER`); internal names keep "Apex". Write
"Apex Radiance" in visible text through `APEX_PRODUCT_NAME`, never the old "S3SS Apex" / "Apex Edition".

History: reorganised on 2026-09-28 for players (short plain words, one feature per card, developer items on one
Developer page); the same day it got a design polish (spacing scale, row descriptions, dividers, tinted notes, button
styles), a full copy rewrite, and the grouped sidebar with tabbed pages below. Later that day (approved from a mockup):
search, changed markers with per-setting Reset, the undo toast, Profiles, peek, hold to compare, GPU cost
chips, "Reload save" badges, inline "Turn on" dependencies, the status bar, the collapsible sidebar,
colour tracks and keyboard use (sections below).

## Files
- `apex_gui.cpp`: window, header, sidebar, pages, search results, Profiles, undo toast, status
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
| Overview (layout-dashboard) | Sims3SettingsSetter recommendation card (only while S3SS is not loaded and `[ui] recommend_s3ss` is true; "Download", "Don't show again"). An **All effects** card sits at the top with one switch and a live on / partly on / off summary; it controls the main effect rows and the whole Performance group without adding a saved key. Lighting, Image and Performance stay as separate cards. The Performance card has one switch for all 12 performance features; clicking its name opens SYSTEM > Performance. |
| WORLD > Lighting (moon-star) | Tabs **Lamps** (Night Lights card: master switch `NightTerrainRelight` + "Lamp color" Pink ... Warm white with a colour track, swatch and "Reload save" badge), **Ground** (Ground & Lots: street lamps light lots, lot lamps light the street, smooth ground light, BRIGHTNESS), **Objects** (every option shown, groups LAMP LIGHT and DOORS, COUNTERS AND FENCES; "Light stairs, railings, columns" has the "Reload save" badge; the DOORS group needs two Ground options: a note naming the missing one(s) and a primary "Turn it on" / "Turn both on"), **Buildings** (Buildings card: groups WALLS and ROOFS; Rooms at Night card (moon, since 2026-09-29): "Darker unlit rooms" + Light left, Blue tint, Soft light on furniture), **Stories** (since 2026-09-29, the user asked for everything about stories in its own tab; Stories card (layers): "Upper floors light the ground" = the separate `SplitLevelGroundLight` feature, shown on and disabled with "Already handled by Sims3SettingsSetter ..." when S3SS's own fix is on; "Outdoor light between floors"; "Seamless walls between floors" and "Indoor light between floors" (Experimental), both disabled with a "Needs ..." note while "Outdoor light between floors" is off). While Night Lights is off, the other tabs show a note and a primary "Turn on Night Lights" button. |
| WORLD > Water & Snow (waves-horizontal) | One page with three cards: Lamp Glow (lamp lighting on ponds), Water Reflections (`reflexoNoLago`, with the Night Lights / Depth Blur requirements and their "Turn on ..." actions), and Snow (sidewalk visibility where Sims have walked). The snow control needs Night Lights and "Street lamps light lots". |
| IMAGE > Color (palette) | Tabs Basic, Tones, Color, Detail (Picture) and Banding (30/09). Banding: the Banding Fix card (blend icon; on by default; Strength 0-100%, Moving grain; Smooth gradients = Picture's deband, which follows the Banding Fix switch and runs with Picture off too; a note; Developer > Debug views: coverage counters and "Show covered surfaces"; features/banding-fix.md). Picture tabs: the Picture card header above the rows (switch = `[qol.picture] enabled`; GPU cost chip; hold to compare (eye) and before / after (columns-2) buttons, disabled while Picture is off, never saved). Tabs **Basic** (brightness, contrast, saturation, temperature, sharpness, smooth gradients), **Tones** (midtones, shadows, highlights, blacks), **Color** (tint, vibrance; FILM TONES = split toning, hue sliders on a hue-circle track with a swatch; COLOR MIXER), **Detail** (clarity, vignette, vignette size). Rows stay visible, greyed out, while Picture is off. |
| IMAGE > Ambient Occlusion (contrast) | New configurations start with the scene AO enabled at Strength 168%, Distance 351 m, High quality, Reach 130% and Keep lamp light 38%; Also in map view is on. Note: turn off the game's own Edge Smoothing; performance note (gauge) "Heavier on the graphics card than other effects: lower the Quality if the game slows down". Advanced includes "Show the shade alone" (not saved); a separate **Sim Occlusion** card below uses the Lucide User Round icon and a standard switch without an Experimental badge. It is off by default. When enabled, Sim intensity and Maximum darkening start at 47%, Hair intensity at 38%, and Transparent hair is on; Advanced also contains Show Sim coverage (not saved). Turning the Sim switch off hides these controls. No page or card reset buttons. Developer > Debug views: status, GPU cost, camera read-out, "Show the shade alone". See features/ambient-occlusion.md. |
| IMAGE > Depth Blur (aperture) | Note: turn off the game's own Edge Smoothing. Depth Blur card (GPU cost chip): Focus Auto / Fixed (segmented), Blur amount (%); Auto: Sharp area Small / Medium / Large; Fixed: Distance Near / Medium / Far + "Fine-tune distance" (0-100% of 0..0.5) + Transition; Sharp in map view; Advanced (rare knobs): Strength, Quality, Focus speed (Auto only, "0.3 s"), Blur the sky, Glowing lights. Mode-specific rows are drawn (and searchable) only in their mode. |
| SYSTEM > Edge Smoothing | A single page with SMAA/FXAA controls and the existing game-MSAA compatibility notice. Window/monitor selection, Apex FPS/V-Sync and VRR status are removed. Legacy profile display sections are ignored without moving category bits. |
| SYSTEM > Performance (gauge; added 2026-09-29) | One card "Performance" ("Fewer stutters while you play"; `PerformanceCard`, no header switch; [features/performance.md](features/performance.md)): 12 individual switch rows, none marked Experimental and all on by default when no explicit setting is saved. The dependent "Remember missing files" row stays under "Faster game file lookups"; the "Use several cores" row stays under "Faster texture compression". "Lot lighting time while moving" remains its 1-15 ms slider (default 3); other tuning stays in Developer. Overview groups the 12 switches as a single Performance row and the All effects switch includes that group. |
| SYSTEM > Report a problem (bug; unified build) | Restored v2.5.3: Capture session hero, always-visible recording/report/point/snapshot tools, dated capture list with Open and two-click confirmed Delete, and sending instructions. No stages, capture/library tabs or compulsory notes. Busy guards and conditional failed-save retry remain; older redesign notes below are historical. |
| SYSTEM > Developer (wrench; development build only) | ImGui tabs: Lighting (Night Lights status, census (list-checks), diagnostics (stethoscope), light probe (crosshair), counters, the generic list of every option; Every-Story Ground Light state), Profiler (activity; Frame Profiler, the Apex shaders line, then the "Performance" dev card: resource lookup cache counters (with the "Remember missing files" and file list cache lines), "Check 1 answer in N against the game", "Check every answer for 10 s", lot lighting call / camera / budget lines, the wall shading gate lines with the slider "Longest wait of a pass while moving (ms)" (0-10000, default 2000), the texture / cache compression lines, the scene node budget lines with the sliders "Nodes per frame while moving" (8-4096, default 512), "ms per frame while moving" (0.1-10, default 2.0) and "Longest wait (ms)" (16-5000, default 500), and the object lookup index lines with "Check 1 answer in N against the game" (default 64) and "Check every answer for 10 s"; none of these is saved), Capture (camera; Frame Capture), Debug views (bug; Edge Smoothing status / GPU cost / red pixels, Depth Blur status / focus mode and depth / GPU cost / "Show blur amount" / far plane, Picture's 8-bit note and GPU cost). |
| SYSTEM > Settings (settings) | Tabs **Menu** (control rows: menu key + Change, text size - 100% + Reset steps, Save now), **Shortcuts** (menu/action key rows, optional filtered screenshot shortcut and temporary game-UI hiding), **Profiles** (see "Profiles"), **Compatibility** (rows Game, Sims3SettingsSetter Installed (circle-check) / Not installed, Features; optional recommendations for DXVK/S3SS; "Details" = the raw S3SS summary and settings migration note), **About** (name, version and build in the header; CREDITS). |

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
and the sidebar version 0.87x muted; group and sidebar labels use the regular menu font at normal size, muted, without artificial letter spacing.

**Icons**: on card headers (18, accent), the sidebar (18), overview rows (18), notes (14), action buttons (20), pills (14).
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
  `SegmentedRow(label, description, id, current, labels, count, tooltips, icons, defaultIndex)` and
  `SelectRow(label, description, id, current, labels, count, width, defaultIndex)` for standard dropdowns; its compact
  Lucide chevron is drawn inside the neutral field instead of a full-height native arrow button;
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
later): legacy `welcome_done` and `key_chosen` fields (preserved but no longer gate startup); `sidebar_collapsed`
(default false). Feature state changes go through the features' own paths (`ApexPatch`, `NotifySettingChanged`,
`PatchManager` unsaved flag, `Picture::SetParams`, `Borderless::SetMode`), so autosave works as before.

### Search
Header field "Search settings" (search icon, "Ctrl+F" hint while empty; Esc or the x clears it). Ctrl+F focuses it,
only while the menu window has keyboard focus (then ImGui captures the keyboard, so the game never sees it). While the
query is not empty the content shows **Search**: one card with every matching row, live (the real controls). Matching:
every word of the query (case-insensitive, ASCII) must appear in the row's visible label or its description. How: the
page tabs are functions; `SearchResults` calls each one (`SearchParts`, in sidebar order: Lighting tabs, Water & Snow,
Color header + Color tabs, Depth Blur, Display tabs, Performance, Settings > Menu and Shortcuts) between `ApexUi::BeginFilter` and
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

### First start and startup hint
A new installation starts with Apex's existing feature defaults and the default menu key (Ctrl+Shift+F11). It does not ask the player to choose a key, install another mod, or complete a tour. Two seconds after the first D3D Present, the existing non-interactive hint appears at the top center: **Apex Radiance is ready · press Ctrl+Shift+F11**. It fades after eight seconds on screen or when the menu opens. The hint is shown for existing and new configs, regardless of the legacy `key_chosen` flag; the existing Settings > Menu switch can hide it. Optional shortcut editing stays in Settings > Shortcuts.

Settings > Compatibility retains its optional DXVK and Sims3SettingsSetter recommendations; they no longer interrupt startup. **Lighting > Buildings > Rooms at Night** shows the Sims3SettingsSetter compatibility card only when official S3SS is loaded. The player explicitly chooses **Back up and correct**; Apex then backs up S3SS.toml and disables only the saved room-light RGB override. Enabling Rooms at Night never writes S3SS.toml. The details explain the exact setting and backup, and a successful correction also updates the affected ambient family for the active session.

**Shortcuts (03/10).** Every Apex action shortcut is intercepted by the overlay before the game sees it. Preset ids and
serialized values remain stable (`letters`, `numbers`, `fkeys`, `mine`); the visible names are **Letters**, **Numbers**, **F keys**, and **Custom**. A missing `[ui] hotkey_preset` still means the earlier Function keys layout. Selecting a
preset is an explicit user action; opening Settings never replaces a saved key or applies a preset.

| | Letter row | Number row | Function row |
|---|---|---|---|
| Menu (`toggle_key`) | Ctrl+Shift+R | Ctrl+Shift+1 | Ctrl+Shift+F11 |
| Compare with the game | Ctrl+Shift+T | Ctrl+Shift+2 | Ctrl+Shift+F10 |
| Refresh the lighting | Ctrl+Shift+G | Ctrl+Shift+3 | Ctrl+Shift+F9 |
| Dev: Light Probe / Light Diag / recorder / Frame Capture | V / B / X / F | 4 / 5 / 6 / 7 | F7 / F8 / F6 / F5 |

Settings > Shortcuts uses a compact full-width **Shortcuts** card: one preset dropdown followed by single-line
editable menu, compare, refresh and screenshot shortcut rows. The screenshot row is disabled while the screenshot
feature is off. Presets change only on explicit selection; opening the card keeps all saved bindings. The former
QWERTY map and preset tiles are removed, avoiding the extra key geometry, tooltips and per-key action scans.
Report shortcuts have their own card; screenshot options and in-menu controls keep their existing cards.
Recording waits until held keys are released, Esc cancels, and a note explains collisions and reserved shortcuts.
The **Custom** state keeps the selected preset as the fallback for unchanged actions. Selecting a preset explicitly
clears per-action overrides; loading Settings never does. Search keeps the standard shortcut rows and enabled-state
behavior. Serialized ids, key defaults and profile fields are unchanged.

Letter row groups nearby left-hand keys; Number row is easy to recall; Function row preserves the earlier layout. Compare turns Night
Lighting, Depth Blur, Edge Smoothing and Picture off and back (not saved; a note shows at the top while off). Refresh
does what the Developer buttons "Rebuild terrain light now" and "Relight lots now" do plus every room and the object
rigs (`NightLighting::RefreshAll`). Per-action choices are stored in `[ui]`: existing `compare_key` and `refresh_key`,
plus `probe_key`, `diagnostics_key`, `recorder_key`, and `frame_capture_key`. Missing new fields mean “use the selected
preset”, preserving older config files. The same fields are included in the optional Shortcuts section of saved
profiles. Settings > Shortcuts has **Use Apex screenshot shortcut**, enabled by default on C only for new or missing key
settings. Existing saved screenshot keys remain unchanged. Apex consumes C and saves one filtered PNG to the game's
standard Documents `Electronic Arts\The Sims 3\Screenshots` folder; it does not also invoke the native screenshot.
After `Ctrl+Shift+C` opens the game's cheat console, all keys pass through until Enter, Esc, or `Ctrl+Shift+C` closes
the console, so typing a cheat never triggers the screenshot shortcut.
Bare F10 remains the game's interface toggle; Apex uses it internally only while hiding the interface for a shot. It
reads the finished back buffer after Apex's scene and Picture passes. Ctrl+Shift+F10 remains Compare. **Hide game UI**
is on by default and temporarily toggles F10, captures one frame and restores the prior tracked state. Apex's own
overlay is suppressed for that frame. This is separate from Report's diagnostic screenshots.
Search, peek, and Picture compare bindings are also saved under `[ui]` and included in the optional Shortcuts profile
section; loading an older config or profile that lacks them keeps their defaults.

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

## Performance grouping (2.5.3)
Four Violet cards keep main controls visible: Camera and lighting (room queue, moving lot budget, wall shading, scene setup); Files and objects (resource cache, missing resources, file lists, object index); Textures and Sims (DXT, several cores, cache compression, Sim sorting); Memory handling. Existing feature descriptions remain on hover; no setting keys or defaults changed. Experimental badges removed at the user's request.

## Guided development diagnosis (development build, pending release)
The Developer page starts with a Violet guide card: select rooms/floors, camera stutters,
dark objects, visual effects or missing translations, then follow Prepare / Reproduce /
Save evidence. Selection opens the relevant existing tool tab without enabling diagnostics
or modifying settings. Open relevant tools returns to that tab at any time.
Capture-session controls stay visible in the guide: Begin, Save report and End, plus Open
captures folder. Reports reuse the existing capture APIs and current session folder.
Profiler measurements retain their separate report control; a capture report does not
export profiler results. Lighting, Profiler, Capture, Debug views and Language retain all
existing tools. Debug-view guidance asks for one view at a time; it does not enforce mutual
exclusion. Guide selection is session-only presentation state and is not saved to TOML.
The public build still hides Developer. No game hooks or lighting policies change.
Profiler performance counters are grouped as Files and objects, Camera and lighting,
and Textures and compression; all original controls are retained. The Language tab
uses a Translation checks card. Lighting and Debug views include visible test guidance.

### Defaults available to every user
The shared public/development menu has a reset entry point on every page. Feature pages reset their entire page, including its internal tabs, after confirmation. Water & Snow resets only its four registered water/snow settings and preserves lighting and upper-floor settings. Display resets edge smoothing and the Apex-managed window mode; externally managed window modes remain under their owner's control. Settings resets menu preferences and shortcuts while preserving the Report diagnostic screenshot preference. Report resets its screenshot preference without deleting any files. Overview and Developer link to the whole-mod reset in Settings > Menu because their contents span features and runtime diagnostics.
The whole-mod reset restores registered feature defaults, Picture, Apex window mode and UI preferences/shortcuts. Legacy welcome/key setup flags are retained in older configs but no longer control any prompts. Undo restores feature/window state and UI preferences. Captures, reports and saved profiles are never deleted. These controls are translated into English, Portuguese, Spanish and French. Runtime-only developer diagnostics are outside the persisted feature reset scope.

### Guided Report page (published 2.5.4)
Report a problem uses a primary choice/capture/finish card, with all original tools and saved captures in advanced sections. Object capture uses a temporary Violet target at the mouse, the configured key and Esc cancellation. It measures a pixel rather than identifying an entire object. The shared public/private UI preserves existing capture APIs and filenames; see features/bug-reports.md.

### Consolidated private development workspace
The private Developer page now uses horizontal Start here, Lighting, Performance, Captures, Visual effects and Translations tabs. Each task has its own explanatory card and retains the existing diagnostic renderers. There is no second sidebar. This replaces the earlier guided tab arrangement described above. Developer stays excluded from the public build. Technical diagnostic output retains engine terminology.

### 2.5.4 public interface scope

The public hotfix includes the shared page/whole-mod default controls, Report capture controls and one-click object-point selection with centered notices. Developer tabs, profiler tools and private diagnostic controls remain excluded by `kPublicBuild`. The simpler Capture / Saved files layout is implemented only in the local `2.5.4-test-report-library` working tree; it is not in the published 2.5.4 release. The four-language translation checks passed; broader gameplay capture/cancel, loading-gate, performance and display-scale testing remains necessary.

### Local overlay availability and clock (`2.5.4-test-report-library`)

Panel opening, shortcuts and notices wait for a loaded active session plus three seconds of readiness; Night Lights uses its world-live signal, with a documented read-only WorldManager fallback when lighting is off. No timer based only on application startup unlocks the menu. The window procedure reads cached availability. Overlay delta time is sampled on every game frame, so a closed-menu gap does not contaminate the FPS average. Genuine low FPS remains visible. A throttled slow-panel phase trace is logged for gameplay diagnosis. The user later reported smooth operation again; the earlier system-wide slowdown's cause remains unconfirmed. See the Report feature doc for tests and fallback limits.

### Compact footer and credits (local test)
The footer shows only configuration save status and Hold Alt to peek. Sims3SettingsSetter detection stays in Settings > Compatibility rather than every page. About credits start with Apex Radiance by @loinyx; the former promotional paragraph is replaced by a short sims3fiend framework-design attribution in all four languages. Historical attribution and third-party licenses remain in the project documentation. Compatibility detection, conflict protection, and integration behavior are unchanged.

### Centered notice width, icons and spacing (local test)

Report screenshots with the Apex menu open now use an explicit `filteredSceneBeforeOverlay` stage: PostScene and
Picture's scene copy finish, Picture runs once, the screenshot is copied, then the menu draws. The normal late Picture
call consumes no second pass. This includes colour filters in report photos and removes callback-registration order
from their capture timing. Player photos on C still read the finished backbuffer at Present. The reported F10/photo
visual mismatch is not considered resolved without a matched in-game comparison; the colour-difference UI mask remains
a heuristic, and partial native interaction menus may still need investigation.

Capture, recording, comparison and entry pills have a content-measured width set before ImGui Begin; only height auto-resizes. Every pill keeps its complete text on one line, without ellipses; width grows with the measured copy. Icon notices use the approved separated capsule: fixed 44-unit height, a 48-unit icon compartment, 20-unit Lucide icon, 22-unit subtle separator, 14-unit gap and 18-unit right inset. Text and icons center independently within the same fixed height. Explicit width and top-left positioning keep new windows centered on their first frame. The entry notice keeps its existing logo layout. Drawing adds only one separator line; no blur or new scene pass is used. Capture notices use Camera for a saved screenshot, CircleCheck for completed captures, TriangleAlert for warnings and Save while writing; light aiming uses Crosshair and active recording uses a pulsing Activity icon. Comparison keeps Columns2; entry retains the logo/Sparkles fallback. Notices enter over 140 ms with a small downward fade and leave over 120 ms; the animation changes only ImGui alpha and position, with no per-pixel effect or added scene work. Recommendation/key setup headers use Info/Keyboard. Existing top-center placement, lifetimes and input pass-through remain.

## Unified build and optional developer mode (2026-10-02)

Published since 2.5.5: one ASI contains the player features and optional developer tools. Enable developer mode in Settings > Menu after confirmation, then restart the game. Default off. Profiles optionally include Development; the save option is hidden in normal mode and appears when importing a profile containing it. Profiles never start measurements or recordings automatically. See [developer-mode.md](features/developer-mode.md). FXAA is first and recommended; SMAA is spatial only. There is no Window page in the current release.

## Historical RC synchronization controls (removed before 2.5.5)

Display > Window retains Borderless and adds experimental V-Sync policy, optional Apex FPS limiting and a dependent target slider. They share the Window profile category and page reset. Restore synchronization resets only these three values. S3SS FPS conflict blocks pacing and supplies an inline explanation. Driver VRR activation and DXVK overrides are explained inline; no automatic claim of VRR support or flicker elimination. See [features/presentation.md](features/presentation.md).

## Local Sim occlusion UI revision

The Ambient Occlusion page has two cards: scene AO first (Strength, Distance, Quality), then Sim Occlusion (body/hair intensity, maximum darkening and advanced coverage preview). The second depends on scene AO and is off by default; it uses the dedicated User Round icon and a standard switch without an Experimental badge, and its controls are hidden while the switch is off. Page and card restore-default buttons, including Overview, Picture/mixer, Depth Blur and Edge Smoothing, are removed. Individual row defaults and the explicit global Settings reset remain. See [features/ambient-occlusion.md](features/ambient-occlusion.md).

Screenshot capture copy shows the configured intercepted key (C by default), explains that it produces one filtered photo without a duplicate, and notes that cheat-console typing passes through. The existing Hide game UI in screenshots switch controls temporary native F10 hiding and restoration; off includes the game UI. The destination note uses the shared small description font. Persistence fields and saved key choices remain unchanged.

Native F10 routing: unmodified F10 down/up messages bypass Apex shortcuts and ImGui keyboard capture, including while the menu is open. Ctrl+Shift+F10 retains its configured compare behavior. Synthetic screenshot keys are observed after forwarding to the game procedure; screenshots wait for that observation before their clean-frame delay, with a two-second cancellation bound. This acknowledges message delivery, not the native HUD state; the visibility tracker remains an estimate and gameplay validation is required.

Screenshot capture is the second card in Settings > Menu. It includes the same shortcut recorder as Shortcuts, with existing conflict and reserved-key validation. Choosing a different key frees C for the native game screenshot. With the default C key, Apex recognizes the forwarded `Ctrl+Shift+C` cheat-console toggle and passes game keys through until Enter, Esc or the toggle closes the console. Keys and capture settings retain their existing persistence fields.

Loading startup: Picture and the shared AO/AA/Depth Blur chain defer their passes until registered shader precompilation completes. The readiness probe uses try-lock and never waits for compiler work. This can delay filters during startup; saved settings are unchanged. Other startup costs and shader consumers remain separate, and gameplay timing needs validation.

Section typography update: shared section labels use the regular menu font at normal size, with no artificial glyph spacing. Overview headings are consistently Lighting, Image and Performance.

Lighting Overview now uses full-width icon/name/description rows for Subtle, Soft, Natural and derived Custom, preserving preset values and Undo choice. Presets affect only lamp intensities on the Lighting page; Water & Snow settings, including pond lamp-glow brightness, remain untouched. Custom is derived from the Lighting values alone and cannot replace individual settings. The redundant fine-tuning card is removed; area tabs remain.

Report capture-session refinement: uses the sidebar Bug icon without a decorative disk, a quieter border, connected outlined step markers with consistent vertical gaps, and a subtle divider above Start a session. Session collection, saving dependencies and active-session controls remain unchanged.

Developer opens directly in Lighting; the redundant Start here overview was removed. Performance, Captures, Visual effects and Translations remain. Session start uses Camera and active-session status uses Activity instead of a plain dot.

> Local development: capture titles are required to finish the post-save form; descriptions remain optional. Whitespace-only titles cannot be saved. Capture files already written remain preserved.

Local development: Profiles separates saving the current setup and the saved library into two standard cards. Existing TOML files, category bits, selective apply, replacement/deletion confirmations and Undo stay unchanged. Capture session uses the shared CardHeader alignment and accent color; its smaller numbered circles use a violet fill and centered text.

Profile icons: the name field has a curated 30-icon Lucide picker inspired by The Sims. The selection is stored as an optional stable name in [meta].icon, not an enum index. Missing or unknown names display Bookmark. Loading settings ignores this visual metadata; category masks and all previous TOML sections remain unchanged. Lighting choices share Overview row typography, description scale, icon column and spacing. The Night Lights overview header no longer has an empty divider.

Profiles now uses the approved aligned selection grid: icon and label left, checkbox right, two equal columns with a one-column fallback based on translated text width. Save and selective load share this layout. Presentation order does not change category bits; Shortcuts remains opt-in.

Profile selection labels match the corresponding tabs: Lighting, Color, Ambient Occlusion, Depth Blur, Edge Smoothing, Performance, Shortcuts and Developer. Lighting retains its existing combined category, including water/snow lighting; no serialized bits or settings tables change.

Saved profiles use Apply in both selection and confirmation states. The control-row widget accepts an optional icon, centered on the same measured text block as the action buttons. Its original full-width row origin is retained for separators and spacing; profile icons no longer use a separate inline item.

### Shared control sizing (2026-10-03)

Approved control concept 02: compact inputs, combos and action buttons use a 36-unit frame; primary form/action groups use 44 units. These reference heights follow the existing UI scale, retaining the Segoe UI font. `ControlSizeScope` adjusts frame padding for the whole group and restores it automatically, so inputs, icon selectors, Cancel and Save share a centre and height. Colour remains independent of size: Apply/Delete in saved-profile rows stay compact, even when confirming an action. Profile saving, capture notes and session start/end use primary groups. Button icons use 20 units, an 8-unit text gap and 12-unit horizontal padding; the profile selector uses the same icon size and padding. Header/pill/notice icon tokens remain separate. No settings or profile schema changes. Runtime visual validation is still required.

Alignment audit: segmented controls now inherit the compact frame height and shared icon/padding tokens; search uses the shared 20-unit icon and 12-unit inset. Diagnostic checkboxes keep independent 20-unit boxes, and former SmallButton actions use the standard compact widget. The profile save group wraps its action when there is insufficient width. The icon-picker popup explicitly restores compact sizing. Developer activation confirmation uses a primary footer group. Both menu skill copies document these distinctions.

Control rows measure the label/description block before vertical centring in taller frames. If the controls leave less than 120 reference units for text, the right-hand group moves below the text with an 8-unit gap; the full row still owns its separator.

### Popup style lifetime correction and size refinement

The player requested slightly smaller controls after the first game test: the current compact/primary heights are 34/40 reference units (superseding the initial 36/44 proposal). Popup-local size scopes now restore frame padding before `EndPopup`, including the profile icon picker and capture/developer modals. The prior scope lifetime let ImGui recover a missing pop and later over-pop the parent form style, changing neighbouring input/button heights. Multi-frame native regression checks are required for this path; compilation alone cannot validate it.

Validation: the native control fixture passed 40 multi-frame cases across EN/PT/ES/FR and scales 1.0/1.4, checking popup style stacks, stable neighbouring input/button heights, button glyph centres within 1.1 pixels and consistent one-pixel divider origins. The native report fixture passed 24 notice and 40 page cases. Release x86 compiled with zero warnings. These checks do not replace final in-game screenshot inspection. The external frontend-design guidance was read and applied within the player's existing Segoe UI/violet design direction.

Shared menu polish: control rows accept an optional measured control height for smaller chips while retaining the normal row minimum, correcting the report probe shortcut position. Custom text/icon buttons and switches repaint the keyboard focus cursor above their own fills; disabled state and hit areas remain unchanged. Report capture descriptions are shorter in all four languages. These shared widget changes apply throughout the feature pages; no settings/profile persistence changes.

Current refinement after player feedback: compact controls are 30 reference units, with 18-unit icons, a 6-unit icon/text gap and 10-unit horizontal padding. Primary groups are 36 units with 14-unit padding, reserved for profile saving, capture completion, session start/end and developer activation confirmation. Both measured button widths and profile icon selector widths follow the active group padding. This supersedes the interim 34/40 sizing; diagnostics, feature tuning and saved-profile row actions use compact controls.

Capture-list right edge: action widths use the current Delete/confirmation label and actual item spacing. Active-session rows omit the unavailable Delete width. Hairlines explicitly end at the window/card work rectangle, independent of the previous action cluster.

Shortcut buttons and their control rows share `KeyChipWidth`, including translated recording prompts; the former 180-unit reservation around a 150-unit button is removed.

### Cross-page design polish

Settings > Menu keeps language/text/start-note controls first, screenshot capture second, then a separate Settings and maintenance card for saving, developer mode and reset. Game Edge Smoothing prerequisite notes on AO/Depth Blur/Edge Smoothing appear only while the actual game AA conflict is active. Remaining raw diagnostic action buttons across feature files and shortcut key buttons use the shared compact action renderer. Segmented choices share visible-glyph vertical centring with buttons; settings keys, defaults, profile bits and renderer behavior are unchanged. Overview, Lighting, Water/Snow, Color, AO, Depth Blur, Edge Smoothing, Performance, Report, Developer and Settings retain their established page/card hierarchy, with common control geometry applied throughout.

### Information hierarchy review (local development)

The approved Violet palette, Segoe UI family and 30/36-unit control roles remain the baseline. Page headings identify the task; card headings identify the affected area; row descriptions explain the result rather than repeating default values already available through reset controls.

- Lighting > Overview retains the three presets and Custom; they change only Lighting-page lamp intensities, and Undo appears only after a choice that can actually be undone. Water & Snow settings are independent.
- Ground separates its three behavior switches (and infrequent dusk Updates) from the four intensity sliders. Objects separates outdoor objects, connected pieces and indoor objects, with Armchair, Fence and Lightbulb icons. Existing disabled dependencies and reload/experimental badges remain attached to their controls.
- Stories keeps the ground/outdoor/indoor sharing controls visible; seam handling and all-floor detail live in Floor detail. Buildings retains its wall/roof groups and separate room ambience card. Water/Snow retain their small, distinct cards. Brightness copy is shorter in all four languages.
- Color keeps basic tint and vibrance immediately visible. Film tones and the six-channel Color mixer are independently expandable, preserving live drag and save-on-release behavior. Search traverses collapsed content through the shared advanced widget.
- AO retains separate world and Sim controls; the GPU-cost note appears while world AO is enabled. Depth Blur and Edge Smoothing retain their existing focus/method hierarchy and advanced controls.
- Overview, Performance, Profiles, Shortcuts and Developer were reviewed against their existing task groups; their approved organization is retained. Settings uses appearance, capture and maintenance cards; its startup hint copy is shorter. Report retains the approved session/capture/library/help flow and required-title modal.

Disclosure text uses the same visible-glyph centring and keyboard focus treatment as action controls. No TOML key, profile bit, preset value, range, reset value, rendering hook or background task is changed. Native shared-control checks are geometric evidence, not a substitute for inspecting every page in the game.

Profile action refinement: shared button icons are 16 reference units. Saved-profile actions read Delete then Apply, with Apply at the right edge; the part-selection state reads Cancel then Apply. Header and row icon sizes are unchanged.

Profile category checkboxes use the shared 20-unit checkbox geometry and ImGui frame/checkmark colours, borders and rounding. The full category row remains clickable with a keyboard focus outline. Category bits and the approved two-column grid are unchanged.

Saved-profile application uses the approved contained-editor layout: profile identity above a nested selection card, heading/count, unchanged category grid, then a footer explaining unselected settings with Cancel followed by Apply. Footer control sizing ends before the nested card closes. Loading guards, optional shortcut/developer selection, activation confirmation and Undo behavior are retained.

Contained profile-editor polish: 8 units separate profile identity and the nested card. The compact header/count and footer are separated from the grid by the shared card divider. Category row height is independent of surrounding primary/compact frame padding, icons use their measured size plus the standard text gap, and the final category row does not add an extra divider. Footer help uses description typography; Cancel and Apply are compact.

## Approved Developer redesign (local development)

The five existing tabs remain Lighting, Performance, Captures, Visual effects and Translations. Lighting now uses evidence, comparison, refresh and inspection cards, with provider/surface readouts, terrain events, texture probes and original individual tests retained in expandable groups. Performance keeps live profiler results visible and groups measurement setup and collected timing details; validation tools retain their current feature gates and settings. Captures retains real sessions, report/light capture actions and the library, with frame operations and their file/shortcut details separated. Visual effects retain every diagnostic view and capture action, while camera, focus, shader and rendering readouts are expandable. Translations separates collection/actions, missing text and placeholder checks, using the actual language selector and real collected errors.

The HTML preview used example state only. The native implementation uses the existing game diagnostics; no mock numbers or unsupported temporal controls are introduced. Existing hooks, action functions, setting keys/defaults, diagnostic persistence, activation/restart rules and profile behavior are unchanged. Compact tool controls and primary collection actions use shared geometry. Build and shared native UI checks do not establish full visual or diagnostic validation inside the game.

Performance tool bodies (follow-up): cached file answers, object lookups, texture/cache compression and scene budgets now put validation controls first and complete original verbose counters in Live counters. Native integer semantics, bounds and setter functions are retained through a responsive row wrapper. Long counter descriptions wrap instead of clipping. Texture-worker readouts have their own disclosure; shader preparation has a card instead of a loose page divider. Original diagnostics remain available.

Refresh lighting is also available without Developer mode in Lighting > Overview, below the balance presets. It reuses the existing terrain/lot request flags and is disabled until the world and lighting are active; no refresh is automatic. The same card is reused by Developer. Copy describes recalculating visibly incorrect or stale lighting instead of investigating/capturing a bug.

The shared Refresh lighting card also provides Refresh lights (Lightbulb), using the same existing full refresh path as Ctrl+Shift+F9 (terrain, rooms, lots and object rigs).

Sim Occlusion coverage preview uses blue for pixels adjusted by Sim controls, green for hair controls and black for original scene AO. A black region is outside the separate controls, not necessarily a region without AO. The preview remains temporary; defaults, profiles and the separate Sim/hair intensity sliders are unchanged.

Profile apply selection opens with a 140 ms smoothstep opacity transition; its layout remains at the final size. The footer uses the regular description face at kSmallScale. Advanced sections immediately after CardDivider reuse that divider rather than drawing a second hairline.
