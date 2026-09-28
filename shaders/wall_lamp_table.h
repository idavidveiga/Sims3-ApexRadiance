// Outdoor wall pixel shaders of the game's ExteriorWall technique (Shaders_Win32.precomp) and the constant that scales
// their baked lamp light ("texld rA, vT, s2" then "mad rB.xyz, rA, cK.x, rC"). Generated offline (scratchpad snowcover/test11.cpp over the 58 ExteriorWall pixel shaders of the precomp: the baked
// light map is the texld at TEXCOORD1, or the one 2D texld read by "mad ..., cK.x"; K is read nowhere else); see lot_light_bridge.cpp
// (WallLampConst). size in bytes, hash = FNV-1a 32-bit over the bytecode DWORDs.
#pragma once
#include <cstdint>

struct WallLampEntry {
    uint32_t size;
    uint32_t hash;
    uint32_t constant;
};

inline constexpr WallLampEntry kWallLampTable[] = {
    {592, 0x01628471, 2}, // ExteriorWall_PS_10146.bin
    {652, 0x3C506DD6, 2}, // ExteriorWall_PS_10149.bin
    {964, 0x85C12A20, 3}, // ExteriorWall_PS_10151.bin
    {1024, 0x6043B8CA, 3}, // ExteriorWall_PS_10153.bin
    {828, 0xE3FA0259, 2}, // ExteriorWall_PS_10155.bin
    {888, 0x3268A650, 2}, // ExteriorWall_PS_10157.bin
    {1188, 0x14C3A05A, 3}, // ExteriorWall_PS_10159.bin
    {1248, 0x8CB0B2B4, 3}, // ExteriorWall_PS_10161.bin
    {656, 0x7EE9D4AE, 3}, // ExteriorWall_PS_10163.bin
    {744, 0x2AAC8ECF, 3}, // ExteriorWall_PS_10165.bin
    {1400, 0x7B070BF1, 3}, // ExteriorWall_PS_10167.bin
    {1340, 0x3B3A3FDA, 3}, // ExteriorWall_PS_10169.bin
    {1024, 0x3981F0B4, 2}, // ExteriorWall_PS_10171.bin
    {964, 0xE0EB5264, 2}, // ExteriorWall_PS_10173.bin
    {1128, 0x0D060ADB, 3}, // ExteriorWall_PS_10175.bin
    {1068, 0xD8AAF2DA, 3}, // ExteriorWall_PS_10177.bin
    {740, 0xABC28037, 2}, // ExteriorWall_PS_10179.bin
    {680, 0x4DB432AD, 2}, // ExteriorWall_PS_10181.bin
    {716, 0x4CC435E6, 2}, // ExteriorWall_PS_10182.bin
    {776, 0x95979E12, 2}, // ExteriorWall_PS_10183.bin
    {1104, 0x41171A5A, 3}, // ExteriorWall_PS_10184.bin
    {1164, 0x6EF60797, 3}, // ExteriorWall_PS_10185.bin
    {1024, 0xC7BF16B6, 2}, // ExteriorWall_PS_10186.bin
    {1084, 0xD10F9072, 2}, // ExteriorWall_PS_10187.bin
    {1400, 0x77C3D046, 3}, // ExteriorWall_PS_10188.bin
    {1460, 0x927AA931, 3}, // ExteriorWall_PS_10189.bin
    {780, 0x30DE669A, 3}, // ExteriorWall_PS_10190.bin
    {624, 0x9B15A316, 2}, // ExteriorWall_PS_1093.bin
    {684, 0x1F1BC902, 2}, // ExteriorWall_PS_1098.bin
    {996, 0x0D11E23A, 3}, // ExteriorWall_PS_1100.bin
    {1056, 0xB7A6D34D, 3}, // ExteriorWall_PS_1102.bin
    {860, 0x4D44E680, 2}, // ExteriorWall_PS_1104.bin
    {920, 0x216B3228, 2}, // ExteriorWall_PS_1106.bin
    {1220, 0x8E44740D, 3}, // ExteriorWall_PS_1108.bin
    {1280, 0xAD7A2F32, 3}, // ExteriorWall_PS_1110.bin
    {688, 0xABF57BC0, 3}, // ExteriorWall_PS_1112.bin
    {776, 0x535E7F95, 3}, // ExteriorWall_PS_1114.bin
    {1432, 0x8605B1F4, 3}, // ExteriorWall_PS_1117.bin
    {1372, 0x04956FE9, 3}, // ExteriorWall_PS_1119.bin
    {1056, 0x65D24F46, 2}, // ExteriorWall_PS_1121.bin
    {996, 0x0C4B45DC, 2}, // ExteriorWall_PS_1123.bin
    {1160, 0x45BD3900, 3}, // ExteriorWall_PS_1125.bin
    {1100, 0x682B244B, 3}, // ExteriorWall_PS_1127.bin
    {772, 0xF12533E0, 2}, // ExteriorWall_PS_1129.bin
    {712, 0x8421C0B2, 2}, // ExteriorWall_PS_1131.bin
    {748, 0xBB7EACB2, 2}, // ExteriorWall_PS_1132.bin
    {808, 0x3B4C2A3D, 2}, // ExteriorWall_PS_1133.bin
    {1136, 0x7CF6FB0B, 3}, // ExteriorWall_PS_1134.bin
    {1196, 0x8CB36F97, 3}, // ExteriorWall_PS_1135.bin
    {1056, 0xCDFF2C12, 2}, // ExteriorWall_PS_1136.bin
    {1116, 0x00459A59, 2}, // ExteriorWall_PS_1137.bin
    {1408, 0x6DE2922B, 3}, // ExteriorWall_PS_1138.bin
    {1468, 0x41316897, 3}, // ExteriorWall_PS_1139.bin
    {812, 0xD3778608, 3}, // ExteriorWall_PS_1140.bin
    {468, 0x25C7EB55, 2}, // ExteriorWall_PS_8628.bin
    {532, 0xF157F02B, 3}, // ExteriorWall_PS_8632.bin
    {496, 0xC6A0248E, 2}, // ExteriorWall_PS_8633.bin
    {560, 0xB972081D, 3}, // ExteriorWall_PS_8635.bin
};
