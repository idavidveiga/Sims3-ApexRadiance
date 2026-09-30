#pragma once
// The Sims 3's CAS triangle sort, rewritten (Apex Radiance, feature "FastCasSort"; docs/features/performance.md, "Faster
// Sim Building").
//
// Game side (TS3W.exe Steam 1.67.2, research\engine_map\full.asm; decompiled re\out\fn_005d1960.c):
//   0x005D1960  "CAS/ModelBuilder/TriangleSortDataList", cdecl(u16* indices, u8* vertices, u32 indexCount, u32 vertexCount,
//               u16 stride, u8 positionOffset), plain ret. Called once per mesh part by FUN_005d3760
//               ("CAS/ModelBuilder/FillDrawable", CALL 0x005D38F3) when the part asks for sorted triangles (alpha meshes:
//               hair, clothing layers). For each triangle t (indexCount / 3 of them, a list of nodes {next, prev, u16 i[3],
//               u32 count}): P_k = (x, y, z, x) / w of its vertices k = 0..2, from the four signed 16-bit numbers at
//               vertices + i_k * stride + positionOffset (x, y, z, w; converted with cvtsi2ss, divided with divps);
//               n1, n2 = the unit vectors from P0 to P1 and P2 (length through rsqrtps and two Newton steps
//               r' = (1 - r r L) (0.5 r) + r); C = a cross product of n2 and n1 (shufps 0x12 / 9). Then for EVERY vertex v
//               of the part (index v & 0xFFFF): d = Q_v - P0, its unit vector the same way, s = sum of (d r) C in the order
//               (z + y) + x, and count++ when s > FLT_EPSILON (comiss: never for NaN). The list is then merge-sorted
//               (0x005CC2D0 with the merge 0x005C9DE0: a node of the second half goes first only when its count is
//               larger (unsigned): a stable sort, larger counts first) and the indices are written back in that order.
//               Triangles x vertices steps of divps + rsqrtps: the "CAS SimService" hitches (30/09: 38 in 10 minutes,
//               ~80 ms each, 83% of their samples in this function).
//   Constants: 1.0 (0x0107A538), 0.5 (0x00F9A5AC), FLT_EPSILON (0x00FE3474).
//
// This file is pure code (SSE intrinsics, x86 and x64) shared by the ASI (features/fast_cas.cpp) and the offline test
// (tools/cas_sort_test):
//   Ref::   the game's function, one vertex at a time with the game's own vector layout and instruction order (the oracle
//           of the offline test; the ASI checks its results against the game's own function in game).
//   Fast::  the same counts: every vertex's Q = (x, y, z) / w computed once (the same divps), then four vertices per SSE
//           instruction, each lane doing exactly the operations the game does for one vertex (same operands, same
//           grouping; rsqrtps per lane gives the same approximation as on a broadcast value), and the triangles can be
//           split over several threads (CountRange). The counts, and so the sorted indices, are the game's bit for bit,
//           given the caller's MXCSR (the ASI's workers copy it).
#include <cstdint>
#include <vector>

namespace CasTriSort {

struct Input {
    const uint16_t* indices = nullptr; // indexCount entries, 3 per triangle
    const uint8_t* vertices = nullptr;
    uint32_t indexCount = 0, vertexCount = 0;
    uint32_t stride = 0;       // u16 in the game's call
    uint32_t positionOffset = 0; // u8 in the game's call
};

inline uint32_t Triangles(const Input& in) { return in.indexCount / 3; }

namespace Ref {
// The whole game function: indices sorted in place (the first 3 * (indexCount / 3) entries)
void Sort(const Input& in, uint16_t* indices);
// The count of one triangle (the oracle of the tests)
uint32_t Count(const Input& in, uint32_t triangle);
} // namespace Ref

namespace Fast {
// Per-vertex positions, structure of arrays, 16-byte aligned, padded to a multiple of 4 (the padding never counts)
struct Prepared {
    std::vector<float> storage;
    float *x = nullptr, *y = nullptr, *z = nullptr;
    uint32_t vertices = 0; // min(vertexCount, 65536): the game indexes vertex v & 0xFFFF
    uint32_t padded = 0;
};
// Q_v = (x, y, z) / w of every vertex the game reads (the same divps as the game)
void Prepare(const Input& in, Prepared& p);
// counts[t] for the triangles [first, last): the game's count, bit for bit. Thread-safe (reads only in / p).
void CountRange(const Input& in, const Prepared& p, uint32_t first, uint32_t last, uint32_t* counts);
// The game's stable sort (larger counts first) and the write-back of the indices
void Finish(const Input& in, const uint32_t* counts, uint16_t* indices);
} // namespace Fast

} // namespace CasTriSort
