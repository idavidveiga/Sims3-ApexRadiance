#pragma once
// Build flavours of Apex Radiance (by @loinyx).
// - Development build (default): adds the developer tools (Ctrl+Shift+F7 light probe, Ctrl+Shift+F8 light diagnostics,
//   Ctrl+Shift+F9 frame capture, census / false colour, the Frame Profiler) and the "Developer" sections of each feature.
// - Public release (MSBuild property ApexPublic=true, which defines S3SS_PUBLIC): those tools and sections are left out
//   (the Frame Profiler is not compiled at all).
// All text is English in both builds; S3SS_TR(pt, en) is kept so older call sites still compile.
#define S3SS_TR(pt, en) en
#ifdef S3SS_PUBLIC
inline constexpr bool kPublicBuild = true;
#else
inline constexpr bool kPublicBuild = false;
#endif
