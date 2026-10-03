> Current RC, 2026-10-02: window modes, monitor selection, Apex FPS/V-Sync control and driver VRR detection were removed at the user’s request. This document is historical. Apex now forwards the game’s original CreateDevice/Reset/Present requests without changing synchronization or window state. G-SYNC/FreeSync remains controlled by the driver/monitor; active VRR in this game is not asserted.

# Display and fluency — private RC, 2026-10-02

This implements approved proposal 3 in the unified ASI, version `2.5.4-rc-display-fluency`. It is not published or installed. It extends the existing private RC; lighting, terrain and image pipelines are unchanged.

## Player flow

Display > Display and fluency has two columns: **How the game appears** (mode and monitor) on the left, and separate **Smooth movement** (Apex FPS) and **G-SYNC / FreeSync** cards on the right. Cards stack on narrow panels. Main controls stay visible; V-Sync, ownership details, CPU wait timings and vendor diagnostics are expandable. Overview and search lead to this same page. Texts are English, Portuguese (Brazil), Spanish and French, with Lucide vector icons.

Four choices are visible: Window, Borderless window, Borderless fullscreen and Game fullscreen. Legacy `off` remains available as Keep game mode under details, preserving older profiles. Window restores the Windows frame rather than leaving a previous popup style behind. Window changes are posted to the window thread, preserve the game backbuffer size, do not resize other displays and respect minimization. Failed posts clear the pending flag.

The chosen mode and current mode are displayed separately. **Game fullscreen describes D3D9 parameters**, not proof of DXVK/Vulkan exclusive scanout. Crossing windowed/exclusive device state needs a successful game device reset/restart; the bootstrap retries original game parameters if the new request fails. No render-thread device reset is forced.

Monitor selection applies to windowed modes. It does not change the game's D3D adapter; game fullscreen uses that adapter's display. Missing saved monitors fall back to the current monitor. The original preference is retained for reconnection. Monitor identity uses the Windows display name, not the order of enumeration. Negative desktop positions are supported. Changing displays refreshes the inventory and invalidates VRR results, including queries in flight.

## Settings and profiles

| `[display]` key | Default | Values | Application |
|---|---|---|---|
| mode | borderless_fullscreen | off, windowed, borderless_windowed, borderless_fullscreen, exclusive_fullscreen | style/position live if device already windowed; device mode on successful creation/reset |
| monitor | empty string | Windows GDI display name; empty follows the window | live for windowed modes |
| vsync_policy | 0 | 0 preserve, 1 on, 2 off | successful device creation/reset |
| frame_pacing | false | boolean | live, guarded when S3SS has a foreground FPS limit configured |
| target_fps | 120 | integer 30–240 | live |

The Window profile part includes these fields. Old profiles that omit monitor or synchronization preserve current values. Page/global reset clears the monitor preference and restores the listed defaults. Undo captures the same profile state. Saved monitor names are machine-specific; an unavailable imported name uses the safe fallback.

## Read-only VRR query

`features/display_monitor.cpp` loads optional **system-directory** driver libraries. No driver DLL is bundled, no driver profile is written and no runtime renderer is replaced.

NVIDIA: public `NvAPI_Disp_GetVRRInfo`, x86 `nvapi.dll`; GDI name is resolved through `NvAPI_DISP_GetDisplayIdByDisplayName`. The 24-byte V1 ABI and public interface IDs come from NVIDIA's MIT SDK. Success with `bIsVRREnabled` reports Enabled on this monitor. Success with that bit clear reports **Not active when checked**, never falsely claiming that the user disabled G-SYNC in the Control Panel. Extra raw flags are retained only as diagnostic evidence. They do not establish per-process engagement.

AMD: documented x86 **ADL2** adapter/display/FreeSync queries are used rather than guessing an ADLX ABI. A separate ADL client context is created/destroyed. A unique connected/mapped logical display must match the Windows display identity. Ambiguous clone/Eyefinity mappings stay unknown. Capability success is required for unsupported status; failures never become unsupported. The Gaming bit from FreeSyncState_Get is a driver configuration report, not proof of game scanout. AMD runtime validation is pending because this machine has NVIDIA hardware.

Driver calls run in one short-lived, module-pinned worker, never DllMain, Present or the menu render thread. The render path reads snapshots. A first visit, reopened page, changed monitor or explicit Check again can request a query; no per-frame polling or automatic setting writes. Closing the page introduces no continual worker. UI results are associated with the actual game-window monitor. Topology invalidation rejects in-flight stale results. A check timestamp is available under help. Unknown, checking, enabled, inactive and confirmed unsupported are distinct.

**Enabled on the display is not evidence that The Sims 3 uses VRR at this moment.** Validate with NVIDIA/AMD's indicator or monitor diagnostics during gameplay, particularly for DXVK and background/foreground transitions.

## Conflicts and ownership

A conditional Conflicts sidebar entry appears only for the existing confirmed runtime MSAA incompatibility: a successfully queried multisampled game backbuffer plus enabled dependent Apex effects. It lists the affected effects and keeps the existing instructions to disable native Edge Smoothing. If resolved, the entry disappears; a currently open page can report no active conflicts.

The S3SS window/FPS conservative guards remain. A guard already preventing Apex's equivalent from running is **ownership information, not two simultaneously active patches**. It therefore does not invent a conflicts entry. Unknown driver overrides, restart pending and VRR disabled/inactive are not conflicts. The displayed S3SS configuration is not a public runtime S3SS API.

Use Apex is the primary violet action to the right of the secondary Keep S3SS FPS action. It expands exact foreground-limit handoff steps. It does not pretend to apply unsupported live changes. Precise, TPS and S3SS's background FPS remain untouched. No S3SS, DXVK, Windows or driver configuration files were edited. Automatic external repair is not implemented in this RC; it needs recognized configuration, backup and subsequent application verification.

## Multiple monitors: practical limits and test

This RC adds correct targeting and honest mode reporting; it does **not** establish a universal fix for FPS loss from a second monitor. Windows composition, the renderer, adapters, driver settings and mismatched refresh rates may affect presentation. Merely selecting the primary display is not measured optimization.

Compare the same save, camera, resolution and one FPS controller: previous RC vs this one, with two monitors connected and with only the game monitor connected. Compare windowed/borderless/game-fullscreen separately, retain Precise/TPS, measure frame-time distributions and camera hitches. Do not infer benefit from the FPS ceiling. DXVK's `dxvk.allowFse` can affect presentation but has compatibility tradeoffs: no override was automatically enabled. Existing `d3d9.presentInterval`/driver V-Sync settings may override the D3D9 request.

Required gameplay checks: Alt-Tab, minimization, switching window mode, DPI differences, moving to secondary display, disconnect/reconnect, loading/reset fallback, profiles/page/global reset, native AA conflict resolution and VRR indicator. Compilation/native test windows do not replace them.

## Local evidence

The x86 test harness linked the actual production monitor/window/presentation modules, with only config/log/S3SS services stubbed. It tested window geometry and caption restoration, parameter fallback status, profile preservation, FPS ownership and eight native ImGui layouts (four languages × two widths). A real read-only NVIDIA query returned success on the AW3225QF at 240 Hz. A second 60 Hz display at a negative desktop origin was enumerated. This is detection/window evidence, not gameplay VRR or frame-time improvement evidence.

Sources: [NVIDIA VRR structure](https://docs.nvidia.com/nvapi/struct___n_v___g_e_t___v_r_r___i_n_f_o___v1.html), [NVAPI SDK](https://github.com/NVIDIA/nvapi), [AMD ADL FreeSync APIs](https://gpuopen-librariesandsdks.github.io/adl/display_8h.html), [AMD ADL SDK](https://github.com/GPUOpen-LibrariesAndSDKs/display-library), [DXVK Windows notes](https://github.com/doitsujin/dxvk/wiki/Windows).
