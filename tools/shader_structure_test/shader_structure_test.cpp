// Offline check of shaders/shader_structure.h against every pixel shader in the game's Shaders_Win32.precomp.
// Read-only: prints to stdout, writes no file.
//   shader_structure_test.exe <Shaders_Win32.precomp> [captured shader .bin ...]
#include "../../shaders/shader_ids.h"
#include "../../shaders/shader_structure.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

using namespace ShaderStructure;

struct Blob {
    size_t offset, bytes;
    std::vector<uint32_t> t;
};

static std::vector<Blob> FindPixelShaders(const std::vector<unsigned char>& d) {
    std::vector<Blob> out;
    for (size_t i = 0; i + 8 <= d.size(); i++) {
        uint32_t v;
        std::memcpy(&v, &d[i], 4);
        if (v != 0xFFFF0300u && v != 0xFFFF0200u && v != 0xFFFF0201u) continue;
        // walk the tokens to the end token
        size_t j = i + 4;
        bool ok = false;
        while (j + 4 <= d.size() && j - i < 65536) {
            uint32_t tok;
            std::memcpy(&tok, &d[j], 4);
            const uint32_t op = tok & 0xFFFF;
            if (tok == 0x0000FFFFu) { ok = true; j += 4; break; }
            if (op == 0xFFFE) j += 4 + 4 * ((tok >> 16) & 0x7FFF);
            else if (tok & 0x80000000u) break; // a register token where an instruction should be
            else j += 4 + 4 * ((tok >> 24) & 0x0F);
        }
        if (!ok) continue;
        Blob b{i, j - i, {}};
        b.t.resize(b.bytes / 4);
        std::memcpy(b.t.data(), &d[i], b.bytes);
        out.push_back(std::move(b));
        i = j - 1;
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: shader_structure_test <precomp> [captured .bin ...]"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
    const auto blobs = FindPixelShaders(d);
    std::printf("%zu pixel shaders in %s\n", blobs.size(), argv[1]);

    struct Family { const char* name; ShaderId dry; StructId id{}; bool have = false; };
    Family fams[] = {{"LotLight", kLotLightPs}, {"WorldMultiLight", kWorldMultiLightPs}, {"WorldCompact", kWorldCompactPs},
                     {"ObjectRig", kObjectRigPs}, {"Roof", kRoofPs}, {"SnowLot", kSnowLotPs}};
    for (Family& fa : fams) {
        for (const Blob& b : blobs)
            if (IsShader(fa.dry, b.t.data(), b.bytes)) {
                fa.have = MakeId(b.t.data(), b.t.size(), fa.id);
                break;
            }
        std::printf("%-16s dry %s; id {%u, 0x%08Xu, 0x%08Xu}\n", fa.name, fa.have ? "found" : "NOT USABLE/NOT FOUND", fa.id.bodyTokens, fa.id.bodyHash, fa.id.finalHash);
    }
    const char* tails[] = {"dry", "wet", "other"};
    for (Family& fa : fams) {
        if (!fa.have) continue;
        std::map<uint32_t, int> seen; // distinct shaders (by hash) matched
        for (const Blob& b : blobs) {
            Match m;
            if (!MatchId(b.t.data(), b.t.size(), fa.id, m)) continue;
            const uint32_t h = ShaderHash(b.t.data(), b.bytes);
            if (seen[h]++) continue;
            std::printf("  %-16s match at 0x%08zX: %4zu bytes hash %08X tail %s%s\n", fa.name, b.offset, b.bytes, h, tails[static_cast<int>(m.tail)],
                        m.tail == Tail::Wet ? (m.wetScale == 5 && m.wetMix == 6 ? " (c5, c6)" : " (other registers)") : "");
        }
        std::printf("  %-16s %zu distinct shaders matched\n", fa.name, seen.size());
    }
    for (int a = 2; a < argc; a++) {
        std::ifstream c(argv[a], std::ios::binary);
        std::vector<unsigned char> cb((std::istreambuf_iterator<char>(c)), {});
        std::vector<uint32_t> t(cb.size() / 4);
        std::memcpy(t.data(), cb.data(), t.size() * 4);
        std::printf("%s (%zu bytes, hash %08X):", argv[a], cb.size(), ShaderHash(cb.data(), cb.size()));
        bool any = false;
        for (Family& fa : fams) {
            Match m;
            if (fa.have && MatchId(t.data(), t.size(), fa.id, m)) {
                std::printf(" %s tail %s (scale c%u, mix c%u)", fa.name, tails[static_cast<int>(m.tail)], m.wetScale, m.wetMix);
                any = true;
            }
        }
        std::puts(any ? "" : " no family");
    }
    return 0;
}
