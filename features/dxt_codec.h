#pragma once
// The Sims 3's CPU DXT1 / DXT5 encoders, rewritten (Apex Radiance, feature "FastTextureCompression";
// docs/features/performance.md, "Faster Texture Compression").
//
// Game side (TS3W.exe Steam 1.67.2, read in research\engine_map\full.asm):
//   0x006152F0  DXT1 image encoder, cdecl(Dst*, Src*): 4x4 blocks in raster order, 8 bytes each
//   0x006154B0  DXT5 image encoder, cdecl(Dst*, Src*): alpha block (8 bytes) + colour block (8 bytes); does nothing unless
//               Src::format is 0x3D or 0x3E
//   helpers (no other callers): 0x00614000 fetch a full 4x4 block as floats, 0x00614C50 fetch a partial block
//   (edge replication), 0x00613F80 fetch the alpha of a partial block, 0x00615100 principal axis + endpoints,
//   0x00614DD0 power-iteration axis (flat-luma blocks), 0x006143F0 colour block (565 rounding, ordered dither, 3/4-colour
//   choice), 0x00614150 alpha block (MMX).
// Both drivers return eax = width & ~3 (no caller reads it).
//
// This file is pure code (SSE2 intrinsics plus one x87 compare in inline assembly, x86 only) shared by the ASI
// (features/fast_dxt.cpp) and the offline test (tools/dxt_test/dxt_test.cpp):
//   Ref::   a literal translation of the game's functions, one block at a time, instruction order kept (comments give
//           the addresses). It is the test oracle.
//   Fast::  the same arithmetic on four blocks at once (one block per SSE lane, "structure of arrays"). Every float
//           operation is the same IEEE operation on the same operands as in the game, with the same association order, so
//           the result is bit-identical whenever no intermediate value is infinite or NaN; the approximate instructions
//           (rcpss / rsqrtss) and the x87 compare are executed per block with the same instruction as the game. A block
//           where an intermediate value is not finite is handed to the caller's fallback (the game's own encoder in the
//           ASI), so the output is the game's in every case.
//   Parallel:: Fast on large images split by block rows over a small persistent pool of worker threads (the calling
//           thread takes part and the call returns when every block is written). Same bytes as Fast for any split: see
//           the namespace below.
#include <cstdint>

namespace DxtCodec {

// The game's image descriptors (callers build them on the stack; 0x14 bytes). Only these fields are read.
struct Dst {
    uint8_t* ptr;     // +0x00 first block
    uint32_t width;   // +0x04 pixels
    uint32_t height;  // +0x08 pixels
    uint32_t pitch;   // +0x0C bytes from one row of blocks to the next
    uint32_t format;  // +0x10 (not read by the encoders)
};
struct Src {
    const uint8_t* ptr; // +0x00 32-bit pixels, byte order B, G, R, A
    uint32_t unused4;
    uint32_t unused8;
    int32_t pitch;      // +0x0C bytes from one pixel row to the next
    uint32_t format;    // +0x10 the DXT5 encoder only runs for 0x3D / 0x3E
};

// Encodes the block whose top-left pixel is `src` (cols x rows pixels, 1..4 each, rows `pitch` bytes apart) exactly as
// the game's encoder does for that block inside a whole image, writing 8 (DXT1) or 16 (DXT5) bytes to `out`.
using BlockFallback = void (*)(void* ctx, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out);

struct FastCounters {
    uint32_t blocks = 0;    // blocks encoded (fallback blocks included)
    uint32_t delegated = 0; // blocks handed to the fallback (an intermediate value was not finite)
    uint32_t powerAxis = 0; // blocks whose axis came from the power iteration (flat-luma blocks)
    uint32_t solid = 0;     // blocks with (almost) no colour variance
};

// True when the CPU has SSE2 (cpuid 1, EDX bit 26). The fast path needs nothing newer.
bool CpuHasSse2();
// For the status line: "SSE2, SSE4.1, AVX2" (whatever cpuid reports)
const char* CpuFeatureText();

namespace Ref {
// The whole-image drivers (0x006152F0 / 0x006154B0)
void EncodeDxt1(const Dst* dst, const Src* src);
void EncodeDxt5(const Dst* dst, const Src* src);
// One block through the drivers above (a 1-block image): the tests' BlockFallback
void EncodeBlock(void* ctx, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out);
} // namespace Ref

namespace Fast {
// Same contract as the game's drivers. `fallback` must be non-null.
void EncodeDxt1(const Dst* dst, const Src* src, BlockFallback fallback, void* ctx, FastCounters* counters);
void EncodeDxt5(const Dst* dst, const Src* src, BlockFallback fallback, void* ctx, FastCounters* counters);
} // namespace Fast

// Large images on several cores. The image's rows of blocks are cut into chunks; the calling thread and up to
// `workers` pool threads take chunks until none is left, then the call returns (synchronous, like the game's function).
// Why the bytes are Fast's, whatever the split:
//   - a block's bytes depend only on its own inputs: its pixels, cols / rows, and (DXT5 right-edge blocks of the last
//     row, the game's alpha-fetch quirk) the driver locals source pointer, y, height and row padding. A chunk is encoded
//     with the WHOLE image's descriptors and absolute block rows, so every block gets exactly the same Job as in the
//     serial loop (same pointers, same y, same height, same padding);
//   - Fast's four SSE lanes never mix (no horizontal arithmetic; rcp / rsqrt / x87 / power iteration per lane), so which
//     blocks share a group (chunk boundaries change the grouping) does not change any block's bytes;
//   - every worker runs the task with the calling thread's x87 control word (the range test's precision) and MXCSR
//     (rounding, FTZ / DAZ), read in the caller and set in the worker before its first chunk, restored after.
// One parallel image at a time: a call made while another thread's image has the pool runs serially on its own thread
// (Result::busy). The fallback may be called on worker threads (it must be thread-safe; the game's encoder is: it only
// uses its stack and constants and is already called from several game threads).
namespace Parallel {
constexpr uint32_t kMaxWorkers = 6;
// min(logical processors this process may use - 2, 6); 0 on 1-2 processor machines (keeps two for the game's own threads,
// one of which is the caller, which also encodes)
uint32_t DefaultWorkers();
struct Options {
    uint32_t workers = 0;             // pool threads besides the caller (0 = serial), at most kMaxWorkers
    uint64_t minPixels = 256u * 256u; // smaller images are encoded serially (thread hand-off would cost more than it saves)
    uint32_t minBlocksPerChunk = 128; // chunks are whole rows of blocks, at least this many blocks (the offline test uses 1)
};
struct Result {
    bool parallel = false;       // split over the pool (false: encoded serially on the calling thread)
    bool busy = false;           // serial because another thread's image had the pool
    uint32_t participants = 1;   // threads that encoded at least one chunk (the caller included)
    uint32_t chunks = 0, chunksByWorkers = 0;
    uint32_t fpStateMismatches = 0; // workers whose x87 control word / MXCSR did not read back as the caller's (must stay 0)
    double workMs = 0.0;         // chunk encode time summed over every thread (serial: the call's time)
    double wallMs = 0.0;         // the call's time on the calling thread
};
// Same contract as Fast::EncodeDxt1/5 (dxt5 selects the format); counters are summed over all threads.
Result Encode(bool dxt5, const Dst* dst, const Src* src, BlockFallback fallback, void* ctx, FastCounters* counters, const Options& options);
// Creates the pool threads now (otherwise at the first large image). Any thread; cheap once they exist.
void Prepare(uint32_t workers);
// Pool threads created so far
uint32_t WorkersCreated();
} // namespace Parallel

} // namespace DxtCodec
