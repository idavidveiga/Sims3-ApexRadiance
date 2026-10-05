// Read-only fixture. Bodies in extracted_resource_paths.h are literal production
// blocks; macros below replace only the D3D interface types with fixtures.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "features/terrain_lighting_policy.h"

struct FakeTexture {
    int id = 0, refs = 1;
    ULONG AddRef() { return ULONG(++refs); }
    ULONG Release() { return ULONG(--refs); }
};
struct FakeShader { int id; };
struct FakeDevice {
    std::array<std::array<float,4>,224> pixel{};
    std::array<std::array<float,4>,256> vertex{};
    std::array<std::array<DWORD,11>,16> sampler{};
    std::array<FakeTexture*,16> textures{};
    FakeShader* ps = nullptr;
    FakeShader* vs = nullptr;
    int writes = 0, pixelWrites = 0, vertexWrites = 0, textureWrites = 0, samplerWrites = 0, shaderWrites = 0;
    int samplerReads = 0, textureReads = 0;
    int failPixelRead = -1, failVertexRead = -1, failSamplerReadAt = -1, failSamplerWriteAt = -1;
    int failTextureReadAt = -1, failTextureWriteAt = -1;
    HRESULT GetPixelShaderConstantF(UINT r, float* v, UINT n) { if(int(r)==failPixelRead) return E_FAIL; std::memcpy(v,pixel[r].data(),n*16); return S_OK; }
    HRESULT GetVertexShaderConstantF(UINT r, float* v, UINT n) { if(int(r)==failVertexRead) return E_FAIL; std::memcpy(v,vertex[r].data(),n*16); return S_OK; }
    HRESULT GetTexture(DWORD s, FakeTexture** v) {
        if(textureReads++==failTextureReadAt) return E_FAIL;
        *v=textures[s]; if(*v) (*v)->AddRef(); return S_OK;
    }
    HRESULT GetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD* v) {
        if(samplerReads++==failSamplerReadAt) return E_FAIL;
        *v=sampler[s][t-1]; return S_OK;
    }
    HRESULT SetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD v) {
        ++writes; if(samplerWrites++==failSamplerWriteAt) return E_FAIL;
        sampler[s][t-1]=v; return S_OK;
    }
    HRESULT SetTexture(DWORD s,FakeTexture* v) {
        ++writes; if(textureWrites++==failTextureWriteAt) return E_FAIL;
        textures[s]=v; return S_OK;
    }
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DPixelShader9 FakeShader
#define IDirect3DVertexShader9 FakeShader
#define IDirect3DTexture9 FakeTexture
#define IDirect3DBaseTexture9 FakeTexture
#include "features/native_terrain_sampler.h"

namespace D3D9Hooks { enum class HookAction { Continue,Skip }; }
std::atomic<float> g_wallGain{1},g_groundGain{1},g_roadGain{1},g_lotMapGain{1},g_night{1};
std::atomic<int> g_wallDrawn{0},g_groundGainDraws{0},g_lotDrawn{0},g_lotMissing{0},g_edgeMatched{0},g_edgeUnmatched{0};
std::atomic<bool> g_softEdges{false},g_wallEnabled{true};
bool g_inOwnCall=false,g_lotRectMiss=false,g_haveLastEdgeRect=false;
std::string g_objDrawInfo;
FakeShader nativePs{1},replacementPs{2};
FakeShader* g_curPs=&nativePs;
FakeShader* g_replacementPs=&replacementPs;
std::map<FakeShader*,DWORD> g_wallConst;
struct ChunkTex { FakeTexture* tex=nullptr; };
std::map<std::pair<int,int>,ChunkTex> g_chunks;
struct LotRect { uint32_t lotHi=0,lotLo=1; float w=32,d=64,tx=0,tz=0; };
LotRect fixtureRect{},g_lastEdgeRect{};
bool fixtureHasRect=false,fixtureCapture=false,fixtureFeather=false;
FakeTexture* fixtureAtlas=nullptr;
std::array<float,4> fixtureAtlasC{.001f,.002f,-.25f,-.5f},lastEdgeUv{};
constexpr float kEdgeBand=2.5f;
namespace LightProbe { bool Capturing() { return fixtureCapture; } }
namespace LightmapSmooth {
FakeTexture* Atlas(float* c) {
    if(!fixtureAtlas) return nullptr; // reproduce production's untouched output on null
    std::memcpy(c,fixtureAtlasC.data(),16); return fixtureAtlas;
}
}
bool Near(float a,float b) { return std::fabs(a-b)<1e-5f; }
std::pair<int,int> Key(float x,float z) { return {int(std::round(x)),int(std::round(z))}; }
FakeTexture* ChunkTexture(const std::pair<int,int>&,FakeTexture* tex) { return tex; }
void EnsureReplacement(FakeDevice*) {}
const LotRect* FindLotRect(const float*,const float*) { return fixtureHasRect?&fixtureRect:nullptr; }
void NoteLotDraw(uint64_t) {}
bool LotEdgeConstants(const float* uv,const float*,const float*,const float*,const LotRect*,float* out) {
    std::memcpy(lastEdgeUv.data(),uv,16); for(int i=0;i<12;++i) out[i]=.02f*float(i+1); return fixtureFeather;
}
void SetPs(FakeDevice* d,FakeShader* ps) { ++d->writes; ++d->shaderWrites; d->ps=ps; }
void SetTex(FakeDevice* d,DWORD s,FakeTexture* tex) { d->SetTexture(s,tex); }
void SetPsConst(FakeDevice* d,UINT r,const float* v,UINT n) { ++d->writes; ++d->pixelWrites; std::memcpy(d->pixel[r].data(),v,n*16); }
void SetVsConst(FakeDevice* d,UINT r,const float* v,UINT n) { ++d->writes; ++d->vertexWrites; std::memcpy(d->vertex[r].data(),v,n*16); }
namespace RigTracker { int fixtureMode=2;int CurrentMode(){return fixtureMode;} }
namespace ShaderPatches {
constexpr unsigned kObjectPixelLamps=8;
struct ObjectPatch {UINT atlasConst=40,strengthConst=41,lampParamConst=42,atlasSampler=9;bool rigLamps=true;};
bool PatchObjectLampPs(std::vector<DWORD>&,ObjectPatch&){return true;}
bool PatchInstancedLamps(std::vector<DWORD>&,ObjectPatch&){return true;}
}
struct PatchedPs { FakeShader* ps=&replacementPs;ShaderPatches::ObjectPatch obj,inst; } objectPatch;
int g_objLampPs=0;
int g_fencePs=0;
std::atomic<bool> g_fenceFix{true};
std::atomic<float> g_fenceStrength{1};
std::atomic<int> g_fenceDrawn{0};
std::atomic<bool> g_objPixel{true},g_objPixelLamps{true};
std::atomic<float> g_objPixelStrength{1},g_objPixelLampStrength{1};
std::atomic<int> g_objLampDrawn{0};
struct ObjectVsInfo {bool contractedLotUv=false;struct {int vertexLight=20,worldK=8;} patched;} objectVsInfo;
ObjectVsInfo* g_curVsInfo=&objectVsInfo;
FakeShader nativeVs{3},replacementVs{4};FakeShader* g_curVs=&nativeVs;
float g_lampData[33][4]{};
template<class Patch> PatchedPs& PatchedFor(FakeDevice*,int&,const char*,Patch){return objectPatch;}
FakeShader* ObjectVsFor(FakeDevice*,FakeShader*){return &replacementVs;}
int SelectLamps(float,float,float){return 1;}
template<class LampArray> std::string DescribeObjectDraw(FakeDevice*,const ShaderPatches::ObjectPatch&,int,bool,int,int,const LampArray&,int){return "fixture";}
void SetVs(FakeDevice* d,FakeShader* shader){++d->writes;d->vs=shader;}
#include "extracted_resource_paths.h"

int checks=0,failures=0;
void Check(bool value,const char* text) { ++checks; if(!value) { ++failures; if(failures<30) std::printf("FAIL: %s\n",text); } }
template<class T> bool Same(const T& a,const T& b) { return !std::memcmp(&a,&b,sizeof(T)); }
void ResetGlobals() {
    objectVsInfo.contractedLotUv=false;
    g_wallGain=1;g_groundGain=1;g_roadGain=1;g_lotMapGain=1;g_night=1;
    g_wallDrawn=0;g_groundGainDraws=0;g_lotDrawn=0;g_lotMissing=0;g_edgeMatched=0;g_edgeUnmatched=0;
    g_softEdges=false;g_wallEnabled=true;g_inOwnCall=false;g_lotRectMiss=false;g_haveLastEdgeRect=false;g_objDrawInfo.clear();
    g_curPs=&nativePs;g_replacementPs=&replacementPs;g_wallConst.clear();g_wallConst[g_curPs]=7;g_chunks.clear();
    fixtureHasRect=false;fixtureCapture=false;fixtureFeather=false;fixtureAtlas=nullptr;lastEdgeUv={};
}
FakeDevice Device() {
    FakeDevice d; d.ps=g_curPs;
    for(size_t r=0;r<d.pixel.size();++r) for(int c=0;c<4;++c) d.pixel[r][c]=float(r*4+c)/100.f;
    for(size_t r=0;r<d.vertex.size();++r) for(int c=0;c<4;++c) d.vertex[r][c]=float(r*4+c)/100.f;
    d.vertex[14]={1.f/256,1.f/256,.5f,.5f};d.vertex[15]={128,0,128,0};
    for(size_t s=0;s<d.sampler.size();++s) for(int c=0;c<11;++c) d.sampler[s][c]=DWORD(s*20+c+1);
    return d;
}
void WallChecks() {
    const int before=checks;
    for(float native: {0.f,-0.f}) for(float gain: {0.f,.25f,1.f,1.5f,4.f}) {
        ResetGlobals();auto d=Device();d.pixel[7]={native,.23f,-.5f,1.25f};const auto original=d.pixel;g_wallGain=gain;int draws=0;
        const bool claimed=DrawWallGain(&d,[&]{++draws;});
        Check(!claimed&&draws==0,"wall zero term leaves native caller to draw");Check(d.writes==0,"wall +/-zero creates no writes");
        Check(Same(d.pixel,original),"wall signed zero and all constants unchanged");Check(!g_inOwnCall&&g_wallDrawn==0,"wall zero does not claim a correction");
    }
    for(int v=1;v<=50;++v) for(int g=0;g<=40;++g) {
        ResetGlobals();auto d=Device();d.pixel[7]={v/20.f,.23f,-.5f,1.25f};const auto original=d.pixel;const float gain=g/10.f;g_wallGain=gain;int draws=0;
        auto expected=original[7];expected[0]*=gain;const bool change=expected[0]!=original[7][0];
        const bool claimed=DrawWallGain(&d,[&]{++draws;Check(g_inOwnCall,"wall draw marked as own call");Check(Same(d.pixel[7],expected),"wall positive term equals previous formula in all four floats");});
        Check(claimed==change&&draws==int(change),"wall exact no-op contract");Check(d.pixelWrites==(change?2:0),"wall changed term writes then restores once");
        Check(Same(d.pixel,original),"wall restores every pixel constant exactly");Check(!g_inOwnCall&&g_wallDrawn==int(change),"wall own-call guard reset and counter correct");
    }
    ResetGlobals();auto d=Device();g_wallGain=1;int draws=0;Check(!DrawWallGain(&d,[&]{++draws;})&&draws==0&&d.writes==0,"gain one skips writes");
    ResetGlobals();d=Device();g_wallGain=2;d.failPixelRead=7;const auto original=d.pixel;
    Check(!DrawWallGain(&d,[&]{++draws;})&&d.writes==0&&draws==0,"wall failed constant read leaves native draw unchanged");
    Check(Same(d.pixel,original)&&!g_inOwnCall&&g_wallDrawn==0,"wall failed read preserves all state and counter");
    ResetGlobals();d=Device();g_wallGain=2;g_wallConst.clear();Check(!DrawWallGain(&d,[]{})&&d.writes==0,"unrecognised wall keeps native draw");
    for(float night: {0.f,.25f,.5f,.75f,1.f}) for(float native: {0.f,.1f,.5f,1.f}) for(float gain: {.25f,1.f,2.021245f,4.f,8.f}) {
        ResetGlobals();d=Device();g_night=night;g_wallGain=gain;g_wallConst[g_curPs]=3;
        d.pixel[3]={native,.188235313f,0.f,0.f};const auto saved=d.pixel;int calls=0;
        const float expected=night==1.f ? native*gain : native*gain+(1.f-night)*std::min(gain,1.f)*.08f;
        const bool claimed=DrawWallGain(&d,[&]{++calls;Check(d.pixel[3][0]==expected,"wall day/twilight scale includes lamp map");
            Check(d.pixel[3][1]==saved[3][1]&&d.pixel[3][2]==saved[3][2]&&d.pixel[3][3]==saved[3][3],"wall changes only lamp x");});
        const bool change=expected!=native;
        Check(claimed==change&&calls==int(change),"wall day/twilight draw contract");
        Check(Same(d.pixel,saved)&&d.pixelWrites==(change?2:0)&&!g_inOwnCall,"wall day/twilight restores constants and guard");
        g_wallEnabled=false;d.writes=0;calls=0;
        Check(!DrawWallGain(&d,[&]{++calls;})&&calls==0&&d.writes==0&&Same(d.pixel,saved),"disabled wall feature preserves native day/twilight/night");
    }
    for(float invalid: {NAN,INFINITY,-1.f}) {
        ResetGlobals();d=Device();g_wallGain=2;g_night=invalid;
        Check(!DrawWallGain(&d,[]{})&&d.writes==0,"invalid wall time factor fails closed");
        ResetGlobals();d=Device();g_wallGain=2;g_night=0;d.pixel[7][0]=invalid;
        Check(!DrawWallGain(&d,[]{})&&d.writes==0,"invalid native wall scale fails closed including NaN");
    }
    std::printf("Extracted DrawWallGain: %d checks\n",checks-before);
}
void LotChecks() {
    const int before=checks;FakeTexture chunk{3},atlas{4},oldTexture{5};
    for(bool recognised: {false,true}) for(int scenario=0;scenario<5;++scenario) {
        ResetGlobals();auto d=Device();objectVsInfo.contractedLotUv=recognised;
        g_chunks[{128,128}]={&chunk};d.vertex[12]={1.f/64,1.f/64,1,1};
        if(scenario==1)d.failVertexRead=12;
        if(scenario==2)d.vertex[12][1]=1.f/32;
        if(scenario==3)d.vertex[12][0]=NAN;
        if(scenario==4)d.vertex[12][0]=0;
        const auto savedPixel=d.pixel;const auto savedVertex=d.vertex;
        const bool align=recognised&&scenario==0;
        Check(ExtractedLotBranch(&d,[&]{
            Check(d.pixel[31][2]==(align?1.f/63:0),"lot inverse scale guarded by verified VS and valid constants");
            Check(d.pixel[31][3]==(align?-(1.f/64)*(16.f/63):0),"unsupported lot UV stays native");
        })==D3D9Hooks::HookAction::Skip,"UV fallback retains normal draw");
        Check(Same(d.pixel,savedPixel)&&Same(d.vertex,savedVertex),"lot UV correction restores constants including invalid values");
    }
    for(bool haveAtlas: {false,true}) for(bool feather: {false,true}) for(float night: {0.f,.25f,1.f}) for(float gain: {1.f,2.f}) {
        ResetGlobals();auto d=Device();g_night=night;g_groundGain=gain;g_lotMapGain=1.3f;fixtureAtlas=haveAtlas?&atlas:nullptr;
        fixtureHasRect=feather;fixtureFeather=feather;fixtureCapture=true;g_softEdges=true;
        g_chunks[{128,128}]={&chunk};d.textures[2]=&oldTexture;
        const auto originalPixel=d.pixel;const auto originalVertex=d.vertex;const auto originalSampler=d.sampler;const auto originalTextures=d.textures;int draws=0;
        const auto result=ExtractedLotBranch(&d,[&]{
            ++draws;Check(g_inOwnCall&&d.ps==&replacementPs,"lot uses replacement once with own-call guard");
            Check(d.textures[2]==(haveAtlas?&atlas:&chunk),"lot selects atlas or native chunk fallback");
            for(int c=0;c<4;++c) {
                const float expected=haveAtlas?(c<2?fixtureAtlasC[c]:fixtureAtlasC[c]+originalVertex[15][c==2?0:2]*fixtureAtlasC[c-2]):originalVertex[14][c];
                Check(d.vertex[14][c]==expected,"lot uses exact mapping or preserves chunk mapping");
            }
            Check(d.pixel[31][0]==LotMapGain()&&d.pixel[31][1]==TerrainLightingPolicy::DayLampScale(night,gain),"lot production gain constants unchanged");
            auto expected=originalPixel[3];expected[0]*=GroundGain();Check(Same(d.pixel[3],expected),"lot c3 equals production night-weighted previous formula");
        });
        Check(result==D3D9Hooks::HookAction::Skip&&draws==1,"lot claims exactly one draw");
        Check(Same(d.pixel,originalPixel)&&Same(d.vertex,originalVertex),"lot restores all PS/VS constants exactly");
        Check(Same(d.sampler,originalSampler)&&Same(d.textures,originalTextures)&&d.ps==g_curPs,"lot restores shaders, sampler states and texture binding");
        Check(oldTexture.refs==1&&chunk.refs==1&&atlas.refs==1,"lot capture reference released exactly");
        Check(!g_inOwnCall&&g_objDrawInfo.empty()&&g_lotDrawn==1,"lot own-call state and diagnostic reset");
        auto uv=haveAtlas?fixtureAtlasC:originalVertex[14];if(haveAtlas){uv[2]+=128*fixtureAtlasC[0];uv[3]+=128*fixtureAtlasC[1];}
        Check(Same(lastEdgeUv,uv),"lot edge fixture receives exact native fallback or atlas mapping");
    }
    ResetGlobals();auto d=Device();int draws=0;
    Check(ExtractedLotBranch(&d,[&]{++draws;})==D3D9Hooks::HookAction::Continue&&draws==0&&d.writes==0,"missing chunk leaves native draw with no writes");
    Check(g_lotMissing==1&&!g_inOwnCall&&g_lotDrawn==0,"missing chunk reports missing without claiming draw");
    ResetGlobals();d=Device();g_chunks[{128,128}]={nullptr};Check(ExtractedLotBranch(&d,[]{})==D3D9Hooks::HookAction::Continue&&d.writes==0,"null chunk texture leaves native draw");
    for(bool haveAtlas: {false,true}) {
        ResetGlobals();d=Device();fixtureAtlas=haveAtlas?&atlas:nullptr;g_chunks[{128,128}]={&chunk};d.failPixelRead=28;fixtureCapture=true;
        const auto pixel=d.pixel;const auto vertex=d.vertex;const auto textures=d.textures;const auto sampler=d.sampler;draws=0;
        Check(ExtractedLotBranch(&d,[&]{++draws;})==D3D9Hooks::HookAction::Continue&&draws==0&&d.writes==0,"saved edge read failure leaves native draw with zero writes");
        Check(Same(d.pixel,pixel)&&Same(d.vertex,vertex)&&Same(d.textures,textures)&&Same(d.sampler,sampler),"saved edge read failure preserves all device states");
        Check(!g_inOwnCall&&g_objDrawInfo.empty()&&g_lotDrawn==0,"saved edge failure has no stale own-call diagnostics");
    }
    ResetGlobals();d=Device();d.failVertexRead=14;Check(ExtractedLotBranch(&d,[]{})==D3D9Hooks::HookAction::Continue&&d.writes==0,"failed terrain UV read leaves native draw");
    ResetGlobals();d=Device();d.vertex[14][0]=.25f;Check(ExtractedLotBranch(&d,[]{})==D3D9Hooks::HookAction::Continue&&d.writes==0,"unsupported terrain UV shape remains native");
    std::printf("Extracted lot branch: %d checks\n",checks-before);
}
void ObjectChecks() {
    const int before=checks;FakeTexture atlas{30},old{31};
    for(float night:{0.f,.5f,1.f})for(float strength:{.5f,1.f,1.7065217f,4.f})for(bool pixel:{false,true})for(int mode:{1,2}){
        ResetGlobals();auto d=Device();fixtureAtlas=&atlas;d.textures[9]=&old;d.vs=&nativeVs;
        g_objPixel=true;g_objPixelLamps=pixel;g_objPixelStrength=strength;g_objPixelLampStrength=strength;
        g_night=night;RigTracker::fixtureMode=mode;g_objLampDrawn=0;
        g_lampData[0][3]=10;g_lampData[16][0]=.2f;g_lampData[16][1]=.1f;g_lampData[16][2]=.05f;
        const auto ps=d.pixel;const auto vs=d.vertex;const auto states=d.sampler;const auto textures=d.textures;int calls=0;
        const float day=std::min(strength,1.f)*.08f;
        const float expected=night==1?strength:day+(strength-day)*night;
        Check(DrawObjectLamp(&d,[&]{++calls;
            Check(d.pixel[41][0]==expected,"actual object draw receives daytime-balanced ground strength");
            Check(d.pixel[42][1]==(pixel?expected:0),"actual object draw receives balanced per-pixel strength");
            Check(Same(d.pixel[0],ps[0]),"object brightness does not change sunlight constants");
            Check(d.ps==&replacementPs&&d.vs==&replacementVs&&g_inOwnCall,"object uses one existing draw with guard");
        })&&calls==1,"object draw claims exactly one callback");
        Check(Same(d.pixel,ps)&&Same(d.vertex,vs)&&Same(d.sampler,states)&&Same(d.textures,textures),"object restores every constant sampler and texture");
        Check(d.ps==g_curPs&&d.vs==g_curVs&&!g_inOwnCall&&g_objDrawInfo.empty()&&g_objLampDrawn==1,"object restores shaders diagnostics and guard");
        Check(atlas.refs==1&&old.refs==1,"object retains exact texture ownership");
    }
    ResetGlobals();auto d=Device();fixtureAtlas=&atlas;g_objPixel=false;int calls=0;
    Check(!DrawObjectLamp(&d,[&]{++calls;})&&calls==0&&d.writes==0,"disabled object path retains native drawing");
    g_objPixel=true;RigTracker::fixtureMode=0;
    Check(!DrawObjectLamp(&d,[&]{++calls;})&&calls==0&&d.writes==0,"indoor objects excluded from daytime outdoor balance");
    std::printf("Extracted DrawObjectLamp: %d checks\n",checks-before);
}
void InstancedChecks() {
    const int before=checks;FakeTexture atlas{32},old{33};
    for(float night:{0.f,.5f,1.f})for(float gain:{.25f,.993083f,1.f,2.f}){
        ResetGlobals();auto d=Device();fixtureAtlas=&atlas;d.textures[9]=&old;
        g_fenceFix=true;g_fenceStrength=gain;g_night=night;g_fenceDrawn=0;
        const auto pixel=d.pixel;const auto vertex=d.vertex;const auto sampler=d.sampler;const auto textures=d.textures;int calls=0;
        const float day=std::min(gain,1.f)*.08f,expected=night==1?gain:day+(gain-day)*night;
        Check(DrawInstanced(&d,[&]{++calls;
            Check(d.pixel[41][0]==expected,"captured bench path gets shared daytime lamp scale");
            Check(Same(d.pixel[0],pixel[0])&&Same(d.pixel[1],pixel[1]),"bench path retains sunlight colour and direction");
        })&&calls==1,"instanced surface draws once");
        Check(Same(d.pixel,pixel)&&Same(d.vertex,vertex)&&Same(d.sampler,sampler)&&Same(d.textures,textures),"instanced surface restores all device states");
        Check(!g_inOwnCall&&d.ps==g_curPs&&g_fenceDrawn==1&&atlas.refs==1&&old.refs==1,"instanced surface restores shader guard counter and ownership");
    }
    ResetGlobals();auto d=Device();g_fenceFix=false;fixtureAtlas=&atlas;
    Check(!DrawInstanced(&d,[]{})&&d.writes==0,"disabled instanced path stays native");
    g_fenceFix=true;fixtureAtlas=nullptr;
    Check(!DrawInstanced(&d,[]{})&&d.writes==0,"missing instanced atlas stays native");
    std::printf("Extracted DrawInstanced: %d checks\n",checks-before);
}
HRESULT NativeSet(FakeDevice* d,DWORD s,FakeTexture* t) { return d->SetTexture(s,t); }
void NativeSamplerChecks() {
    const int before=checks;FakeTexture native{10},previous{11};
    // 22 state reads, 11 state sets, texture get, and texture set are faulted
    // individually. GetTexture returns an owned reference in this fixture.
    for(int fault=-1;fault<35;++fault) {
        auto d=Device();d.textures[3]=&previous;const auto original=d.sampler;const auto originalTextures=d.textures;
        if(fault>=0&&fault<22)d.failSamplerReadAt=fault;
        if(fault>=22&&fault<33)d.failSamplerWriteAt=fault-22;
        if(fault==33)d.failTextureReadAt=0;
        if(fault==34)d.failTextureWriteAt=0;
        {NativeTerrainSampler guard(&d,2,3,&native,NativeSet);
            Check(bool(guard)==(fault==-1),"native alias reports success only with no injected failure");
            if(guard)Check(d.textures[3]==&native&&Same(d.sampler[2],d.sampler[3]),"native alias texture and all eleven states match source");
        }
        Check(Same(d.sampler,original)&&Same(d.textures,originalTextures),"native alias restores texture and every state after all partial failure paths");
        Check(previous.refs==1&&native.refs==1,"native alias releases its captured texture reference after all failure paths");
    }
    auto d=Device();d.textures[3]=&native;d.sampler[3]=d.sampler[2];
    { NativeTerrainSampler guard(&d,2,3,&native,NativeSet);Check(bool(guard),"already aliased sampler succeeds"); }
    Check(d.writes==0&&native.refs==1,"already aliased sampler avoids every setter and releases captured reference");
    { NativeTerrainSampler guard(&d,2,2,&native,NativeSet);Check(!guard&&d.writes==0,"same source and spare sampler fail closed"); }
    { NativeTerrainSampler guard(&d,2,3,nullptr,NativeSet);Check(!guard&&d.writes==0,"null native map fails closed"); }
    std::printf("Production NativeTerrainSampler: %d checks\n",checks-before);
}
int main() {
    WallChecks();LotChecks();NativeSamplerChecks();ObjectChecks();InstancedChecks();
    std::printf("Resource paths: %d checks, %d failures; extracted production, fake D3D9, no game/driver execution\n",checks,failures);
    return failures?1:0;
}
