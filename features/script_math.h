#pragma once
// Faster script math (Apex Radiance, feature "ScriptMath"; Performance page).
//
// The Mono interpreter linked into TS3W.exe (the game's scripts run on it) tests both operands of every floating-point
// compare and branch opcode for NaN through msvcr80!_isnan before comparing them: 25 handlers (0x00E54D1B..0x00E58304 on
// Steam 1.67.2, 05/10), each
//     fld qword [esi-10h]                       DD 46 F0
//     mov ebx, [_isnan import slot]             8B 1D <slot>          \
//     sub esi, 10h | 8                          83 EE xx               |
//     sub esp, 8 ; fstp qword [esp]             83 EC 08 DD 1C 24      | block A (22 bytes)
//     call ebx ; add esp, 8 ; test eax, eax     FF D3 83 C4 08 85 C0  /
//     jnz <nan path>                            0F 85 rel32 | 75 rel8
//     fld qword [esi+8] | [esi]                 DD 46 08 | DD 06
//     sub esp, 8 ; fstp qword [esp] ; call ebx ; add esp, 8 ; test eax, eax     block B (13 bytes)
// Both blocks become the same test without the call: two pushes for the old "sub esp, 8", fucomip st0, st0 (pops; PF = 1
// only for a NaN), jnp +1, inc eax (eax zeroed first in A; already 0 in B). The bytes from the old return address on (A +17
// "add esp, 8 ; test eax, eax", B +8) are kept, so a thread inside _isnan during the switch returns into the same code.
// ebx still receives _isnan's address (mov ebx, imm32) and eax the 0 / 1 _isnan returns: the registers, the x87 stack and
// the flags the jumps read are those of the original; only the dead stack bytes under esp differ. Start checks that the slot holds msvcr80!_isnan and that it returns exactly 0 / 1; no branch in TS3W.exe
// targets the inside of a block (scanned 05/10 on Steam 1.67.2). The sites are found by pattern in .text, so the EA
// build is covered by the same scan.
#include <string>

namespace ScriptMath {

bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();

} // namespace ScriptMath
