#pragma once
#include "ui/i18n.h"
#include <string>
#include <string_view>
namespace DeveloperCopy {
inline const char* English(const char* original) {
 struct Alias {const char* from; const char* to;};
 static constexpr Alias aliases[] = {
 {"Rooms keep their light when their lamps did not change (floor switches)","Keep unchanged room lighting"},
 {"False colour: magenta = gets lamp light but no fix claimed it","Highlight surfaces without a lighting correction"},
 {"Census: write ApexRadiance_Censo.txt","Save lighting coverage report"},
 {"Rebuild terrain light now","Rebuild ground lighting"},
 {"Relight lots now","Recalculate lot lighting"},
 {"Save light diagnostics","Save lighting diagnostics"},
 {"Record story light samples for the diagnostics","Collect lighting samples on each floor"},
 {"Soft lot edges (A/B: off = plain max of lot and ground light)","Test soft lighting at lot edges"},
 {"Smooth the ground light maps on the GPU (A/B: off = CPU worker)","Use the GPU to smooth ground lighting"},
 {"Compare GPU vs CPU (one chunk)","Compare GPU and CPU on one ground section"},
 {"Relight only nearby terrain (lamp changes re-render only the chunks under the changed lamps)","Update ground near changed lamps"},
 {"Paced terrain sweep (dusk and lamp-change rebuilds re-render one chunk at a time, nearest first)","Rebuild ground lighting in small steps"},
 {"Replace with white (unticked = black)","Use white for texture replacement; off uses black"},
 {"Untick all","Restore all probe textures"},
 {"Sample the render thread","Sample the rendering thread"},
 {"Sample the simulation thread","Sample the simulation thread"},
 {"Sampling rate","Samples per second"},
 {"Hitch multiplier","Stutter threshold relative to normal frames"},
 {"Hitch floor","Minimum duration to count as a stutter"},
 {"Count state calls","Count graphics-state changes"},
 {"Write ApexRadiance_Hitches.txt","Write stutters to the measurement file"},
 {"Time lot object building (this session)","Measure object setup on lots for this session"},
 {"Time the Mutex::Lock hook","Measure lock waiting time"},
 {"Per-hook registry timing","Measure each mod graphics callback"},
 {"Enable frame profiler","Collect frame measurements"},
 {"Clear","Clear the collected measurement"},
 {"Save report now","Save the current measurement report"},
 {"Timing run","Use the timing measurement preset"},
 {"Sampling run","Use the code-sampling preset"},
 {"Check 1 answer in N against the game","Check one answer every N lookups"},
 {"Check every answer for 10 s","Check every answer for 10 seconds"},
 {"Longest wait of a pass while moving (ms)","Maximum wall-shading wait while moving (ms)"},
 {"Nodes per frame while moving","Object nodes processed per moving frame"},
 {"ms per frame while moving","Object setup time per moving frame (ms)"},
 {"Longest wait (ms)","Maximum wait for a pending object (ms)"},
 {"Check 1 texture in N against the game","Check one texture every N textures"},
 {"Check every texture for 30 s","Check all textures for 30 seconds"},
 {"Worker threads per texture (0 = one core)","Extra workers per texture; zero uses one core"},
 {"Split textures from (side, pixels)","Minimum texture side for parallel work (pixels)"},
 {"Default","Restore worker defaults"},
 {"Check 1 stream in N by decompressing","Decompress one in every N streams to verify it"},
 {"Also run the game's compressor on 1 stream in N (0 = never; adds its time)","Compare with the game every N streams; zero disables"},
 {"Search depth (candidates per position)","Compression search depth"},
 {"Show smoothed pixels in red","Highlight smoothed pixels in red"},
 {"Show the temporal blend (green = blended, magenta = history dropped)","Show temporal blend: green kept, magenta discarded"},
 {"Swap jitter and subsample pairing","Test the alternative temporal sample pairing"},
 {"Show blur amount","Show where and how strongly blur is applied"},
 {"Far plane","Far-plane distance for fixed focus"},
 {"Show the shade alone","Show only the added ambient shadow"},
 {"Save depth and colour","Save depth and colour images"},
 {"Show covered surfaces","Highlight surfaces corrected for banding"},
 {"Capture now","Capture the next two drawn frames"},
 };
 const std::string_view text(original); const auto end=text.find("##"); const auto visible=text.substr(0,end);
 for(const auto& a:aliases)if(visible==a.from)return a.to;
 return original;
}
inline const char* Label(const char* original) {
 thread_local std::string label;
 label=I18n::Tr(English(original));label+="###";label+=original;return label.c_str();
}
}
