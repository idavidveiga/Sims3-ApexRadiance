#pragma once
// Game-code addresses used by Night Lights (and its parts) and Every-Story Ground Light, for every game build.
//
// Steam 1.67.2 (TS3W.exe 0x52DEC247): the fixed addresses the features were written for (each feature still checks the
// bytes it patches, exactly as before). Any other build (EA app 1.69.47 TS3.exe 0x6707155C, 1.69.43, retail, unknown):
// every address is found at startup by a masked byte signature scanned in the game's own .text in memory (on the EA app
// build .text is encrypted on disk and only decrypted in memory by the EA activation stub, so it cannot be matched from the
// file; the scan runs after the first Present, when it has long been decrypted). An address counts only when its
// signature matches exactly once (or every match gives the same value); a feature whose required addresses are not all
// found stays off with "Not available on <version>: <missing>" and writes nothing.
//
// Every signature is logged ("[Addr] name: N matches at ... (Steam 0x...)", with 16 bytes around each match on non-Steam
// builds) so a user's ApexRadiance_LOG.txt is enough to refine a signature. Table and method: docs/engine/game-versions.md.
#include <cstdint>
#include <initializer_list>
#include <string>

namespace GameAddr {

// Ids that come from one multi-match or callers-of entry are consecutive (sorted by address).
enum class Id : uint16_t {
    // ---- Night Lights core ----
    RootGetter,         // FUN_006e97b0: mov eax,[root]; ...; mov eax,[eax+1C0h]; ret
    RootPtr,            // the global it reads (*(root)+0x1C0 = light manager)
    QueueRoom,          // FUN_006c7160 thiscall(treeLevel, roomId)
    TerrainVisitorSite, // light test in the terrain bake visitor FUN_00c29620
    ArmSiteRemoval,     // light test before "mov [esi+38h],32h" in FUN_006b6090 (3 sites, by address)
    ArmSiteRegister,    //  ... in FUN_006b64b0
    ArmSiteMoved,       //  ... in FUN_006b6590
    EnumLights,         // FUN_006acf70 stdcall(visitor)
    ChunkRenderCall,    // call FUN_00c7e7a0 in the terrain update's per-chunk loop
    ChunkRenderFn,      // FUN_00c7e7a0
    LampColourSite,     // movaps xmm0,[esi+0E0h] in the street lamp evaluation (developer option)
    LotPassSite,        // mov eax,[edi+0D8h] in FUN_00c7f750 (developer option)
    LotPassConst,       // the float constant loaded right after it
    LotPassTexGlobal,   // the texture global its world pass binds
    LotPassNullBind,    // mov eax,[tex]; push 0; push 0 (lot pass binds nothing)
    QualitySite0,       // mov byte [esp+0Ch],0 in FUN_00adb5a0 (2 sites, by address; developer option)
    QualitySite1,       //  ... in FUN_00adb850
    // ---- light classes (light factory FUN_006ac590: type 3..11 -> constructor -> vtable) ----
    LightJumpTable,
    LightVtable3, LightVtable4, LightVtable5, LightVtable6, LightVtable7, LightVtable8, LightVtable9, LightVtable10, LightVtable11,
    LightColour3, LightColour4, LightColour5, LightColour6, LightColour7, LightColour8, LightColour9, LightColour10, LightColour11, // vfunc+0x10
    LightEval3, LightEval4, LightEval5, LightEval6, LightEval7, LightEval8, LightEval9, LightEval10, LightEval11,                   // vfunc+0x4C
    LightPos,           // vfunc+0x24 (the same function in all nine classes)
    // ---- object light bridge ----
    CapOperandSite,     // mov ecx,offset cap in FUN_006b92a0
    CapGlobal,
    RigGatherReturn,    // return address of the light colour call in FUN_006bb270
    LumaWeights,
    DirtyAllRigs,       // FUN_006b58f0 __fastcall(cells)
    RigCtor,            // FUN_006bb8f0
    RigVtable,
    RigCtorCall0, RigCtorCall1, RigCtorCall2, // its three calls in FUN_006f7880
    RoomGatherCall,     // call FUN_006bb2f0 in FUN_006bbde0
    RoomGather,
    CellGather,         // FUN_006b5af0
    RigUpdate,          // FUN_006bbf90 __fastcall(rig)
    SetLightColour,     // FUN_006bda90
    SetColourCall0, SetColourCall1, SetColourCall2, SetColourCall3, SetColourCall4, SetColourCall5, SetColourCall6, // its 7 callers
    ScriptSetColourCall,
    ScriptSetColour,    // FUN_006bc3e0
    // ---- light between stories ----
    AddWorldLights,     // FUN_006c6ab0
    AddWorldLightsCall0, AddWorldLightsCall1,
    LevelGather,        // FUN_006c6990
    LevelGatherCall0, LevelGatherCall1,
    CascadeTest,        // cmp [esi+1A0h],eax; jnz (JNZ at +7) in FUN_006c7250
    RoomByIdCall, RoomById,
    InvalidateCall, InvalidateRoom,
    SetInsertCall, SetInsert,
    SolvePoint,         // LightPointWithAllLights FUN_0069fd60
    SolvePointCall0, SolvePointCall1, SolvePointCall2,
    BatchSolveCall,     // the one in FUN_006a31d0
    LightEvalReturn,    // after "call edx" (light vfunc+0x4C) in SolvePoint
    WallTestCall, WallTest,
    BatchSamples,       // global vector of the wall sample batch
    WallCullBatchFn,    // FUN_006a30b0
    WallCullCall, WallCull,
    // ---- rig tracker ----
    ModelDraw,          // FUN_006f6250
    BinderCall, Binder,
    InstanceFlush,      // FUN_006cf920
    // ---- Every-Story Ground Light ----
    GetLotIdGatherCall, // call GetLotID in the outdoor-room light gather
    GetLotId,           // BaseLight::GetLotID FUN_006bc020
    Count
};

// Resolves the table: fixed addresses on Steam 1.67.2 (the signatures are still scanned and logged as a self-check),
// signature scan elsewhere. Init thread, after the game settled (first Present) and before the features install; on a
// build whose .text still looks encrypted it waits up to 15 s. Safe to call once; later calls do nothing.
void Resolve();
bool Resolved();

// True on Steam 1.67.2: every address is the fixed one and the features keep their exact byte checks.
bool IsFixed();

// The address, or 0 when it was not found (or Resolve has not run).
uintptr_t Get(Id id);
const char* Name(Id id);

// True when every id is known; otherwise `missing` (if given) gets their names, comma separated.
bool Have(std::initializer_list<Id> ids, std::string* missing = nullptr);
// "Not available on <version>: missing <names>"
std::string NotAvailable(const std::string& missing);

// Feature groups (FeatureInfo::gameCodeGroup): "NightLights" (core of Night Lights), "SplitLevel"
bool GroupAvailable(const char* group, std::string* missing = nullptr);

// Light type (3..11) of a light vtable, or -1
int LightTypeOfVtable(uintptr_t vtable);

// Non-Steam builds: once per session, when the first world is loaded (render thread), reads a few known objects and logs
// whether the struct offsets assumed from Steam look right (light manager, night level, light cells, light types).
void CheckWorldStructs();

} // namespace GameAddr
