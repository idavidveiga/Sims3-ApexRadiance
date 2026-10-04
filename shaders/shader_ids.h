#pragma once
// Identifiers of the game's shaders that Night Remake recognises by exact bytecode (Steam 1.67.2.024037): size in bytes
// and FNV-1a 32 over the bytecode's DWORDs. Only the identifiers are kept here, never the game's bytecode itself.
#include <cstdint>
#include <cstring>
#include <vector>

struct ShaderId {
    size_t size;
    uint32_t hash;
};

// pixel shaders
constexpr ShaderId kLotLightPs = {568, 0xFDAD274Bu};   // lot terrain light pass
constexpr ShaderId kWorldMultiLightPs = {756, 0xEC3141ABu}; // summer multi-pass WORLD terrain, F7 2026-10-02
constexpr ShaderId kWorldMultiLightVs = {656, 0x5882F972u}; // s2 lamp UV from c13; chunk matrix c8/c10
constexpr ShaderId kWorldCompactPs = {1296, 0x73376C6Au}; // single diffuse layer WORLD terrain, s3 lamp, c7.x gain; F7 19:55
constexpr ShaderId kWorldCompactVs = {744, 0x34E1F1B7u}; // lamp UV c15; chunk matrix c8/c10, same pair at 19:44
constexpr ShaderId kObjectRigPs = {600, 0x0A2D0BE4u};  // instanced outdoor objects (fences, shrubs)
constexpr ShaderId kRoofPs = {1136, 0x6EC87E3Bu};      // roofs
constexpr ShaderId kLakePs = {1344, 0x4F52846Au};      // lake water (sun shadow read with a hardware depth compare, texldp)
constexpr ShaderId kLakePs2 = {1308, 0xB21E05D4u};     // the same lake water with the sun shadow compared by hand (texld + cmp): the
                                                       // game uses it in other weather (2.5.1 report: reflections gone when not sunny)
constexpr ShaderId kSnowLotPs = {1852, 0x08DF01E8u};   // snowy lot light pass
constexpr ShaderId kRoofSnowPs = {4992, 0x3CEB025Eu};  // snowy roofs
// EA 1.69 Light Probe / day-night census, 2026-10-04: full-day materials of the cinema/theatre facade.
// The main marquee uses D5ED0EF3. The narrow centre panel maps to 4E570819 by the same BFFCCC56 object VS,
// geometry/count correlation and its captured night counterpart 36F5E915. Night shaders are deliberately not listed.
constexpr ShaderId kCinemaMarqueeDayPs = {864, 0xD5ED0EF3u};
constexpr ShaderId kCinemaMarqueePanelDayPs = {500, 0x4E570819u};
constexpr ShaderId kCinemaMarqueeDayVs = {1060, 0xBFFCCC56u};
// vertex shaders
constexpr ShaderId kRoofVs = {1192, 0x1F851ECBu};
constexpr ShaderId kLakeVs = {1088, 0x23CCB61Bu};
constexpr ShaderId kSnowLotVs = {1400, 0x9256F0DFu};
constexpr ShaderId kFloorVs = {1492, 0x2BC34FA8u};     // snowy floor tiles

inline uint32_t ShaderHash(const void* code, size_t bytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t d;
        std::memcpy(&d, static_cast<const unsigned char*>(code) + i, 4);
        h = (h ^ d) * 16777619u;
    }
    return h;
}
inline bool IsShader(const ShaderId& id, const void* code, size_t bytes) { return bytes == id.size && ShaderHash(code, bytes) == id.hash; }
