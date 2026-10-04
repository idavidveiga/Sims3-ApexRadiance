#pragma once
// Development-only diagnostic: captures the raw scene alpha channel at the PostScene boundary, before the game's
// first depth-disabled draw (normally the bloom composite / UI). In The Sims 3 this alpha is the bloom mask for
// walls, objects and roofs. The probe is read-only: it never changes shader constants, render states or output colour.
#include <string>

namespace BloomAlphaProbe {
void Request(float nightLevel); // 0 = day, 1 = night; used only to name/tag the capture
std::string Status();
}
