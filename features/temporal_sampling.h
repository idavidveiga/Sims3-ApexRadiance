#pragma once
// CPU-only clip-space phase compensation; no allocations or GPU reads.
namespace TemporalSampling {
inline void Jittered(const double camera[4][4], int phase, unsigned width, unsigned height, double out[4][4]) {
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) out[r][c]=camera[r][c];
    if (phase < 0 || phase > 1 || !width || !height) return;
    const double x=(phase==0 ? 0.5 : -0.5)/width;
    const double y=(phase==0 ? -0.5 : 0.5)/height;
    for (int c=0;c<4;++c) { out[0][c]+=x*camera[3][c]; out[1][c]+=y*camera[3][c]; }
}
}
