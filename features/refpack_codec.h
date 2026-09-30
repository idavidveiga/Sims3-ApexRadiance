#pragma once
// RefPack ("QFS") streams as The Sims 3 writes and reads them (Apex Radiance, feature "FastCacheCompression";
// docs/features/performance.md, "Faster Cache Compression"). Pure code, shared by the ASI (features/fast_refpack.cpp) and
// the offline test (tools/refpack_test/refpack_test.cpp).
//
// Game side (TS3W.exe Steam 1.67.2, research\engine_map\full.asm):
//   0x004EC200  stream write, thiscall(stream, src, size, dst, capacity, flags), ret 14h; the only reference is the RefPack
//               stream vtable 0x00FB9018 slot +4 (0x00FB901C). dst == 0 && (flags & 1): returns the size bound
//               ((size * 20) >> 4) + 32 with no work. Otherwise 0x004EC0A0(dst, capacity, src, size, [stream+4] allocator,
//               mode) with mode = 1 when flags & 2, 2 when flags & 0x10000, else 0; dst == 0 counts without writing
//               (the package writer 0x004A7030 sizes its buffer that way). Never fails, never reads `capacity`.
//   0x004EC0A0  header + tables: header word 0x10FB (0x90FB and a 4-byte size when size >= 0x1000000), | 0x4000 and a
//               16 KB window (0x3FFF) unless mode & 1 (128 KB window 0x1FFFF); big-endian uncompressed size (3 or 4 bytes).
//               Inputs <= 16 KB: 0x004EB750 with a 256-entry hash (1 KB table on the stack); larger: 0x004EBB90 with a
//               64K-entry hash (256 KB table from the allocator, memset to -1 on every call). Both also allocate the chain
//               array (window + 1) * 4 bytes (512 KB for the 128 KB window): 768 KB per call, freed at the end.
//   0x004EB750 / 0x004EBB90  greedy LZ: 3-byte hash, EVERY candidate of the hash chain inside the window is compared (no
//               depth limit), best = largest (length - opcode bytes); matches never reach the last 4 bytes. mode 2 inserts
//               only the first position of a match, otherwise every position.
//   0x004EB3B0  decompressor, cdecl(dst, capacity, src, srcSize): returns the header's size, 0 on any error. Checks every
//               copy against capacity, source length and the start of dst; needs the 0xFC..0xFF stop opcode; ignores
//               header bit 0x4000. The only RefPack decoder in the exe (called by the stream read 0x004EC010). The
//               official Sims3SettingsSetter replaces THIS function (its "RefPack decompressor"); Apex never touches it.
//
// Opcodes (all produced and accepted identically): 2 bytes 0xxxxxxx (0-3 literals, length 3-10, offset <= 1024),
// 3 bytes 10xxxxxx (length 4-67, offset <= 16384), 4 bytes 110xxxxx (length 5-1028, offset <= 131072), 1 byte 111xxxxx
// (4-112 literals, multiple of 4), stop 111111xx (0-3 literals).
#include <cstddef>
#include <cstdint>

namespace RefPackCodec {

constexpr uint32_t kFailed = 0xFFFFFFFFu; // what the game's callers test for ("cmp eax,-1": store uncompressed / fail the write)

// The size bound the stream returns for (dst == 0, flags & 1): the game's arithmetic, 32-bit
inline uint32_t SizeBound(uint32_t size) { return ((size * 20u) >> 4) + 32u; }

// Header and window the game uses for these flags and size
struct Params {
    uint32_t header;    // big-endian first two bytes
    uint32_t sizeBytes; // 3 or 4
    uint32_t window;    // largest offset allowed (0x3FFF or 0x1FFFF)
};
Params ParamsFor(uint32_t size, uint32_t flags);

// Reusable work memory of the fast compressor (hash heads + chain links). Allocate ContextBytes() bytes (any alignment
// of 16, zero-filled or not), then Init. One context per thread at a time.
struct Context {
    uint32_t* head = nullptr;
    uint32_t* prev = nullptr;
    uint32_t base = 0; // positions of the current call are stored as base + index (older entries are < base: stale)
};
constexpr uint32_t kHashBits = 16;
constexpr uint32_t kPrevBits = 16;
size_t ContextBytes();
void InitContext(Context& ctx, void* memory);

// Search effort of the fast compressor
struct Effort {
    int maxChain = 32;     // hash-chain candidates compared per position
    uint32_t niceLen = 96; // stop searching once a match this long is found
    bool lazy = true;      // look one position ahead before taking a match
};

// Fast compressor with the game's stream format: same header, same window per flags, same opcodes, valid for the game's
// decompressor. dst == nullptr: counts only. capacity == 0: unlimited; otherwise nothing is written past dst + capacity
// and kFailed is returned when the stream does not fit. Returns the stream size (header included).
uint32_t Compress(Context& ctx, const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags, const Effort& effort = Effort{});

// ---- Segmented compression: large inputs on several threads ----
// The input is cut into kSegmentBytes pieces. Each piece is parsed on its own (ParseSegment: its matches may point into
// the window before it, never past its end), in any order and on any thread, into tokens; a SegmentEncoder then writes
// the stream from the tokens, piece after piece, in order. Same header, window, opcodes and stop as Compress (literal
// runs simply continue across pieces). The bytes depend only on the input, the flags and the effort, never on which
// thread parsed which piece, so a counting run and the write give the same stream. Not the same bytes as Compress (each
// piece starts a fresh parse; the size differs by a fraction of a percent).
constexpr uint32_t kSegmentBytes = 128 * 1024;
constexpr uint32_t kMaxSegmentTokens = kSegmentBytes / 3 + 2; // a match is at least 3 bytes
struct Token {
    uint32_t pos;    // where the match starts in the input
    uint32_t lenOff; // length << 17 | (offset - 1)
};
inline uint32_t SegmentCount(uint32_t size) { return (size + kSegmentBytes - 1) / kSegmentBytes; }
// The tokens of piece `index` (at most kMaxSegmentTokens) into `out`; returns their count. One context per thread.
uint32_t ParseSegment(Context& ctx, const uint8_t* src, uint32_t size, uint32_t flags, uint32_t index, const Effort& effort, Token* out);
// Writes the stream: Begin, Add each piece's tokens in order, End (returns the stream size, or kFailed when it does not
// fit a non-zero capacity; nothing is ever written past dst + capacity). dst == nullptr counts.
struct SegmentEncoder {
    uint8_t* dst = nullptr;
    uint32_t cap = 0, n = 0, litFrom = 0, size = 0;
    const uint8_t* src = nullptr;
    bool full = false;
    void Begin(const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags);
    void Add(const Token* tokens, uint32_t count);
    uint32_t End();
};
// The whole segmented compression on the calling thread (the reference for the threaded version, and its fallback)
uint32_t CompressSegmented(Context& ctx, const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t capacity, uint32_t flags, const Effort& effort, Token* scratch);

// 0x004EB3B0 translated: the header's size, 0 on any error (the checks are the game's)
uint32_t Decompress(uint8_t* dst, uint32_t capacity, const uint8_t* src, uint32_t srcSize);

// 0x004EC0A0 + 0x004EB750 / 0x004EBB90 translated (offline tests only: ratio and speed of the game's compressor).
// dst == nullptr counts; allocates its tables per call like the game.
uint32_t GameCompress(const uint8_t* src, uint32_t size, uint8_t* dst, uint32_t flags);

} // namespace RefPackCodec
