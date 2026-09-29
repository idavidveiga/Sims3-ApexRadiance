# Apex Radiance for The Sims 3

A lighting, visuals and performance mod for The Sims 3. At night, street lamps and lot lamps really light the world around them: the ground, lots, objects, fences, walls, roofs, ponds and snow. It also goes after the game's small, frequent stutters, especially while you move the camera and while lots, Sims and textures load, without changing how the game looks. On top of that come color filters, clean anti-aliasing, a soft depth blur and a borderless window, all from one in-game menu.


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


## Features

**World**
- **Night Lights:** lamps light the ground, lots, streets, outdoor objects, fences, walls and roofs, with smooth warm light and no hard edge at lot borders. Lamps on upper floors light the yard too.
- **Water & Snow:** lamps glow and sparkle on ponds, ponds mirror the trees and houses along their shore, and walked-on sidewalks show through the snow.

**Image**
- **Color:** brightness, contrast, saturation, warmth, sharpness, smooth gradients, film-style tones, a color mixer and a vignette. Menus and text keep their normal look.
- **Depth Blur:** softly blurs the distant background, like a camera focused on what's near. It turns off by itself in map view.

**System**
- **Anti-aliasing:** SMAA or FXAA on the 3D world while menus stay sharp.
- **Borderless window:** windowed or fullscreen, without a title bar.
- **Menu:** search, profiles, undo, per-setting reset and a hold-to-compare view.

**Performance** (System > Performance; every option is experimental and has its own switch, so any that misbehaves can be turned off)

On from the start:
- **Lot lighting while the camera moves:** instead of spending up to 15 ms of one frame on lot lighting, the work is spread over several frames while you pan or zoom (about 80% fewer lighting stutters while moving, in testing). When the camera stops, the game's normal budget comes back.
- **Wall shading waits while moving:** the shading of a new lot's walls, a 10 to 17 ms hitch, is done once the camera stops.
- **Fewer big freezes from Night Lights:** lamps that switch or flicker by themselves no longer rebuild the terrain light, and rebuilds never happen while the camera moves.

Off until you turn them on:
- **Faster game file lookups:** the game searches every package (hundreds with mods) one by one each time it needs a texture or model. Apex Radiance remembers where things are: lookups cost about half as much, and the stutters they caused dropped by roughly 70% in testing. Under it, **Remember missing files** also skips the repeated searches for files no package has.
- **Faster file lists:** fewer stutters when Sims load outfits and shapes.
- **Faster texture compression:** the game's texture encoder rewritten with the exact same output, split over several processor cores for large textures (a 2048×2048 texture: about 35 ms down to about 6 ms).
- **Faster cache compression:** a faster compressor for what the game stores in its caches, in the game's own format.
- **Faster object lookups:** less work when lot lights update and for scripts.

Apex Radiance's own shaders are compiled at startup on a background thread, never in the middle of play.

## Requirements

- The Sims 3, Steam version 1.67.2 (`TS3W.exe`). Night Lights and Every-Story Ground Light patch this exact version; other versions show those features as unavailable.
- An ASI loader in `Game\Bin`, for example [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
- Recommended: the official Sims3SettingsSetter for its frame rate limiter and stutter fixes.

## Install

1. Close the game and the launcher.
2. Copy `ApexRadiance.asi` into `The Sims 3\Game\Bin\`.
3. Start the game and press **Ctrl+Shift+F11** to open the menu.

Settings are saved in `Documents\Electronic Arts\The Sims 3\Apex Radiance\ApexRadiance.toml`.

If the game crashes, Apex Radiance writes `ApexRadiance_Crash.txt` in that same folder: please attach it when you report the problem.

If you used the older combined build (Sims3SettingsSetter with Apex inside) or `S3SSApex.asi`, delete it from `Game\Bin` and keep the official `Sims3SettingsSetter.asi`. Apex Radiance copies your old settings on its first start.

## Working with Sims3SettingsSetter

Both mods can be installed together. When they both offer the same thing, Apex Radiance steps aside: the borderless window and the ground lighting on upper floors are left to Sims3SettingsSetter when it has its own version turned on.

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
