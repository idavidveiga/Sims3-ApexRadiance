<p align="center"><img src="docs/images/logo-256.png" width="140" alt="Apex Radiance logo"></p>

# Apex Radiance for The Sims 3

A lighting, visuals and performance mod for The Sims 3. At night, street lamps and lot lamps really light the world around them: the ground, lots, objects, fences, walls, roofs, ponds and snow. It also goes after the game's small, frequent stutters, especially while you move the camera and while lots, Sims and textures load, without changing how the game looks. On top of that come color filters, soft ambient occlusion, clean anti-aliasing, a soft depth blur and a borderless window, all from one in-game menu.


<div align="center">

<table>
<tr>
<td>
<img src="https://github.com/user-attachments/assets/c2fb9c34-8545-49f4-a3ac-a65e9086de86" width="100%">
</td>
<td>
<img src="https://github.com/user-attachments/assets/3d287975-5231-475e-9cb8-37342915c7db" width="100%">
</td>
</tr>

<tr>
<td>
<img src="https://github.com/user-attachments/assets/3b748f76-115e-414e-831c-c55fc2e14134" width="100%">
</td>
<td>
<img src="https://github.com/user-attachments/assets/5902b18d-f29a-44b3-9cec-6817192344ac" width="100%">
</td>
</tr>

<tr>
<td colspan="2">
<img src="https://github.com/user-attachments/assets/82856ded-a8b7-4c23-a3e9-5c3ed1631b40" width="100%">
</td>
</tr>

</table>

</div>


## 🆕 New in 2.3

- 🏠 **Light between floors fixed:** double-height rooms and open floors now light the floor above as soon as the lot loads (no more leaving and entering the lot).
- ⚡ **Faster cache compression, 3x:** big Sim caches are split over several processor cores and never compressed twice.
- 🧍 **Faster Sim building (new, Experimental):** the game's slowest step when it builds a Sim's hair and clothing, many times faster with exactly the same result.

## 🌈 New in 2.2

**Banding Fix (Experimental):** no more color steps (banding) in lamp light, shadows and other smooth gradients. Light now fades smoothly across walls, floors, the ground and objects.

- ✨ An invisible, fixed grain where the game rounds its colors, on **every surface of the 3D world**; menus are untouched.
- 🎚️ New **Color › Banding** tab: the grain's Strength (0-100%), an optional **Moving grain** for high frame rates, and **Smooth gradients**, which now works with Picture off too and also helps the sky.

**Ambient Occlusion, lighter and more flexible:**

- ⚡ About **25-30% lighter** at the same look (4K, in testing).
- 🎚️ **Five qualities**, from Very Low to Ultra, right on the card.
- 🗺️ **Also in map view:** soft shade around houses and trees when the map is open.
- 👁️ **Show the shade alone** (Advanced) to see what it does while you adjust it.

> [!NOTE]
> Ambient Occlusion needs the game's own Edge Smoothing off (Options › Graphics). Apex's anti-aliasing works with it.

📥 **Download:** [Nexus Mods](https://www.nexusmods.com/thesims3/mods/247) · [GitHub release](https://github.com/loinyx/Sims3-ApexRadiance/releases/latest). Every version's notes are on the [Releases](https://github.com/loinyx/Sims3-ApexRadiance/releases) page.

## Features

### Night Lights

In the base game lamps glow but barely light anything around them. Apex Radiance rebuilds night lighting so they really light their surroundings:

- **Ground, lots and streets:** smooth, warm pools of light on grass, sidewalks, roads and lot floors, with no hard edge where a lot ends.
- **Objects, fences, walls and roofs:** outdoor objects, fences, stairs, house walls and roofs pick up the light of nearby lamps.
- **Every-Story Ground Light:** lamps on upper floors, balconies and terraces light the ground below too.
- **Light styles:** Soft, Natural or Bright sets lamp brightness everywhere at once.
- **Brightness per part:** ground, roads and sidewalks, street lamps, lot lamps, objects, fences, walls, roofs and ponds each have their own slider.
- **Lamp colors:** from the game's pink to warm white, applied instantly, with an optional separate color for lot lamps.
- **Moonlight:** choose how much the moon lights the world at night.
- **Inside the house (Experimental):** lamps inside shine through stairwells and open floors to the floor above or below, and stop at solid floors and walls; furniture, stairs and curtains take the room's light smoothly.
- **Rooms at Night (Experimental):** instead of the game's strong blue glow in rooms with every lamp off, a soft background light you set with Brightness and Blue tint, on walls, floors and furniture alike.
- **Changing floors keeps the light:** every floor of the lot you play is lit in full detail, walls meet at the floor line with no step in the light, and switching floors no longer flickers.
- **Fast, correct updates:** placing, moving or deleting a lamp updates the light quickly, without the big freezes the game used to have. The lighting also refreshes itself after loading and after you change a setting, and a **Refresh the lighting** button and shortcut redo it at any time.

### Water & Snow

- Lamps glow and sparkle on ponds and lakes.
- Ponds mirror the trees and houses along their shore (needs Depth Blur on and the game's own Edge Smoothing off).
- Walked-on sidewalks show through the snow.

### Color

A full picture editor for the 3D world; menus and text keep their normal look.

- Brightness, contrast, saturation, warmth and sharpness.
- Film-style tones, a six-color mixer and a vignette.
- A before/after switch and a hold-to-compare button.

### Banding Fix (experimental)

The game rounds its picture to 256 shades per color, so smooth light (a lamp's glow on a wall, a room fading into shadow) shows visible steps. The Banding Fix adds an invisible, fixed grain right where the colors are rounded, so light fades smoothly on every surface of the 3D world; menus are untouched. On by default, with its Strength, an optional Moving grain (a new grain every frame, for high frame rates) and **Smooth gradients** (which also softens the sky) on the Color › Banding tab. If anything looks wrong, turn it off there.

### Ambient Occlusion

Soft shade where things meet: under furniture, in corners, where walls meet the floor and around houses and trees. It is computed at full resolution with no noise, so it stays perfectly still when the camera does, and lamp-lit or bright surfaces keep their light and color. Five qualities from Very Low to Ultra, adjustable strength and reach, and it also works in map view. It is heavier on the graphics card than the other effects, and needs the game's own Edge Smoothing off.

### Depth Blur

Softly blurs the distant background, like a camera focused on what's near. The focus follows what you're looking at, the effect turns itself off in map view, and its quality is adjustable.

### Display

- **Anti-aliasing:** SMAA or FXAA on the 3D world while menus stay sharp, from Low to Extreme (straighter long edges at 4K).
- **Borderless window:** windowed or fullscreen, without a title bar.

### Performance (experimental)

Apex Radiance goes after the game's small, frequent stutters, especially while you move the camera and while lots, Sims and textures load, without changing how the game looks. Every option has its own switch (System > Performance), so any that misbehaves can be turned off.

On from the start:
- **Faster room lighting:** rooms light up sooner when you enter a lot or change floors.
- **Lot lighting while the camera moves:** instead of spending up to 15 ms of one frame on lot lighting, the work is spread over several frames while you pan or zoom (about 80% fewer lighting stutters while moving, in testing). When the camera stops, the game's normal budget comes back.
- **Wall shading waits while moving:** the shading of a new lot's walls, a 10 to 17 ms hitch, is done once the camera stops.
- **Fewer big freezes from Night Lights:** lamps that switch or flicker by themselves no longer rebuild the terrain light, and rebuilds never happen while the camera moves.

Off until you turn them on:
- **Faster game file lookups:** the game searches every package (hundreds with mods) one by one each time it needs a texture or model. Apex Radiance remembers where things are: lookups cost about half as much, and the stutters they caused dropped by roughly 70% in testing. Under it, **Remember missing files** also skips the repeated searches for files no package has.
- **Faster file lists:** fewer stutters when Sims load outfits and shapes.
- **Faster texture compression:** the game's texture encoder rewritten with the exact same output, split over several processor cores for large textures (a 2048×2048 texture: about 35 ms down to about 6 ms).
- **Faster cache compression:** a faster compressor for what the game stores in its caches, in the game's own format; large Sim caches are split over several processor cores (the ~300 ms freezes when Sims are stored drop to ~100 ms, in testing).
- **Faster Sim building:** hair and see-through clothing layers are sorted many times faster, with exactly the same result: fewer hitches in Create a Sim and when Sims change outfits.
- **Faster object lookups:** less work when lot lights update and for scripts.
- **Spread new objects over frames:** while the camera moves, new objects join the scene over a few frames instead of all at once (fewer hitches when a lot streams in).

Apex Radiance's own shaders are compiled at startup on a background thread, never in the middle of play.

### The menu

The first start asks which key opens it (Ctrl+Shift+R, Ctrl+Shift+1 or Ctrl+Shift+F11, or your own), and a note shows that key at every start. The Shortcuts tab sets the other shortcuts, such as **Refresh the lighting**.

- **Four languages:** English, Portuguese, Spanish and French ("Automatic" follows your Windows language).
- **Search:** find any option by name, with or without accents.
- **Profiles:** save your setup, choose which parts go into each profile (Night Lights, Color, Ambient Occlusion, Depth Blur, Edge Smoothing, window mode, Performance) and which parts to apply when you load one. Share them by copying the files from the Profiles folder.
- **Undo** after any change, **per-setting reset**, a dot on everything changed from its default, and **Reset all settings** (Settings > Menu) to start over.
- Adjustable text size.
- A Compatibility page with your game version and the other mods it detects.

## Requirements

- The Sims 3, Steam version 1.67.2 (`TS3W.exe`). The EA App version 1.69 (`TS3.exe`) is supported too (experimental); on other versions, features whose game code is not found show as unavailable.
- An ASI loader in `Game\Bin`, for example [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- Recommended: the official Sims3SettingsSetter for its frame rate limiter and stutter fixes.

## Install

1. Close the game and the launcher.
2. Copy `ApexRadiance.asi` into `The Sims 3\Game\Bin\`.
3. Start the game, pick your menu key when it asks, and press it to open the menu.

Settings are saved in `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance.toml`.

If the game crashes, Apex Radiance writes `ApexRadiance_Crash.txt` in that same folder: please attach it when you report the problem.

If you used the older combined build (Sims3SettingsSetter with Apex inside) or `S3SSApex.asi`, delete it from `Game\Bin` and keep the official `Sims3SettingsSetter.asi`. Apex Radiance copies your old settings on its first start.

## Compatibility

- **Sims3SettingsSetter:** both mods can be installed together, and I recommend using both. When they both offer the same thing, Apex Radiance steps aside: the borderless window and the ground lighting on upper floors are left to Sims3SettingsSetter when it has its own version turned on.
- **Sims 3 Performance Patch:** compatible. The two mods work on different parts of the game (it speeds up loading and saves memory; Apex Radiance targets in-game stutters) and do not patch the same game code.
- Works with DXVK.

## Building

Visual Studio 2022 Build Tools (v143), C++20, Release | x86, static CRT, with vcpkg (`x86-windows-static`) providing Dear ImGui (dx9 + win32), Microsoft Detours and toml++.

```
MSBuild ApexRadiance.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false                     -> Release\ApexRadiance.asi (development build)
MSBuild ApexRadiance.sln /p:Configuration=Release /p:Platform=x86 /p:VcpkgEnableManifest=false /p:ApexPublic=true  -> Public\ApexRadiance.asi (release build)
```

The development build adds measuring and diagnostic tools. How every feature works, including the reverse-engineered engine details, is documented in [`docs/`](docs/README.md).

## Credits

- **Apex Radiance** by [@loinyx](https://github.com/loinyx).
- Sims3SettingsSetter by sims3fiend
- Every-Story Ground Light (lamps on upper floors lighting the ground) uses a technique from [Arro](https://arro-now.tumblr.com/)'s Split-Level Lighting Fix.
- Edge Smoothing's FXAA mode follows FXAA 3.11 by Timothy Lottes (NVIDIA).
- Third-party code: [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Microsoft Detours](https://github.com/microsoft/Detours) (MIT), [toml++](https://github.com/marzer/tomlplusplus) (MIT), [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al. (see `third_party/smaa/LICENSE.txt`), [Lucide](https://lucide.dev) icons (ISC, see `third_party/lucide/LICENSE`).

The Sims is a trademark of Electronic Arts Inc. This is a fan-made mod, not affiliated with or endorsed by Electronic Arts.

## License

[MIT](LICENSE)
