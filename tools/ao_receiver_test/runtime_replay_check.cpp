// Native, game-independent checks of the actual pre-draw Sim receiver replay.
// run_runtime_checks.ps1 extracts the production block into the temporary include.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>
#include "shader_ids.h"
#include "shader_patches.h"

namespace {
struct Params { bool simControls = true, transparentHair = false; float strength = 1, simStrength = 0, hairStrength = 0, simMaxShade = 1; };
struct State { bool active = true, ready = true, showSimMask = false; Params p; } g;
std::atomic<DWORD> simRenderThread{0};
ShaderId kSimReceiverPs[2]{};
ShaderId kSimHairReceiverPs[1]{};
IDirect3DSurface9* sceneDepth = nullptr;
bool failNextPixelSet = false;
namespace DepthShare { IDirect3DSurface9* Surface() { return sceneDepth; } }
namespace ExtraHooks {
HRESULT RawGetDepthStencilSurface(IDirect3DDevice9* d, IDirect3DSurface9** s) { return d->GetDepthStencilSurface(s); }
HRESULT RawSetDepthStencilSurface(IDirect3DDevice9* d, IDirect3DSurface9* s) { return d->SetDepthStencilSurface(s); }
}
namespace D3D9Hooks {
HRESULT CallOriginalCreateVertexShader(IDirect3DDevice9* d, const DWORD* c, IDirect3DVertexShader9** s) { return d->CreateVertexShader(c, s); }
HRESULT CallOriginalCreatePixelShader(IDirect3DDevice9* d, const DWORD* c, IDirect3DPixelShader9** s) { return d->CreatePixelShader(c, s); }
HRESULT CallOriginalSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return d->SetVertexShader(s); }
HRESULT CallOriginalSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    if (failNextPixelSet) { failNextPixelSet = false; return D3DERR_DEVICELOST; }
    return d->SetPixelShader(s);
}
HRESULT CallOriginalSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { return d->SetRenderTarget(i, s); }
}
#define LOG_WARNING(message) ((void)0)
#include "runtime_replay.generated.h"
#undef LOG_WARNING

unsigned checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}
bool Near(float a, float b) { return std::fabs(a - b) <= 2.4e-7f; }
template <typename Device> void CheckProductionMinSupport(Device* device) {
    // Keep the runner usable with SourcePath pointing to the older replay block.
    if constexpr (requires { SupportsSimMinBlend(device); })
        Check(SupportsSimMinBlend(device),"production G32R32F MIN capability helper accepts native support");
}
std::vector<DWORD> Compile(const char* source, const char* entry, const char* profile) {
    ID3DBlob *code = nullptr, *error = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (FAILED(hr)) { if (error) std::printf("%s\n", static_cast<const char*>(error->GetBufferPointer())); SafeRelease(error); return {}; }
    std::vector<DWORD> result(code->GetBufferSize() / 4);
    std::memcpy(result.data(), code->GetBufferPointer(), code->GetBufferSize());
    SafeRelease(code); SafeRelease(error); return result;
}
struct V { float position[4], colour[4]; };
struct Snapshot {
    static constexpr D3DRENDERSTATETYPE types[] = {D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_STENCILWRITEMASK, D3DRS_SCISSORTESTENABLE, D3DRS_ZFUNC,
        D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP};
    DWORD states[std::size(types)]{};
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    explicit Snapshot(IDirect3DDevice9* dev) {
        for (size_t i = 0; i < std::size(types); ++i) dev->GetRenderState(types[i], &states[i]);
        dev->GetViewport(&viewport); dev->GetScissorRect(&scissor); dev->GetVertexShader(&vs); dev->GetPixelShader(&ps);
        dev->GetRenderTarget(0, &rt); dev->GetDepthStencilSurface(&ds);
    }
    bool Matches(IDirect3DDevice9* dev) const {
        Snapshot after(dev);
        return std::memcmp(states, after.states, sizeof(states)) == 0 && std::memcmp(&viewport, &after.viewport, sizeof(viewport)) == 0 &&
               std::memcmp(&scissor, &after.scissor, sizeof(scissor)) == 0 &&
               vs == after.vs && ps == after.ps && rt == after.rt && ds == after.ds;
    }
    ~Snapshot() { SafeRelease(vs); SafeRelease(ps); SafeRelease(rt); SafeRelease(ds); }
};
struct Fixture {
    IDirect3D9* d3d = nullptr; IDirect3DDevice9* dev = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr; IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9 *body = nullptr, *hair = nullptr, *other = nullptr;
    IDirect3DVertexBuffer9* vertices = nullptr; IDirect3DSurface9* bb = nullptr;
    HWND window = nullptr;
    bool overlap = false;
    bool Init() {
        char modulePath[MAX_PATH]{};
        GetModuleFileNameA(GetModuleHandleA("d3d9.dll"),modulePath,MAX_PATH);
        std::printf("D3D9 backend DLL: %s\n",modulePath);
        window = CreateWindowA("STATIC", "Apex receiver checks", WS_OVERLAPPED, 0, 0, 32, 32, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
        pp.BackBufferWidth = pp.BackBufferHeight = 16; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        if (!d3d) return false;
        D3DADAPTER_IDENTIFIER9 adapter{};
        if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT,0,&adapter)))
            std::printf("D3D9 adapter: %s | driver %s\n",adapter.Description,adapter.Driver);
        const HRESULT deviceResult=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
        if (FAILED(deviceResult)) {
            std::printf("D3D9 CreateDevice HAL/windowed 16x16 HRESULT=0x%08lX\n",static_cast<unsigned long>(deviceResult));
            return false;
        }
        const char* material = "struct V { float4 pos:POSITION; float4 col:COLOR0; }; V VS(V v) { return v; } float4 PS(float4 col:COLOR0):COLOR0 { return col; } float4 Hair(float4 col:COLOR0):COLOR0 { return col * float4(0.9,1,1,1); } float4 Other(float4 col:COLOR0):COLOR0 { return col * float4(1,1,1,0.9); }";
        const auto v = Compile(material, "VS", "vs_3_0"), p = Compile(material, "PS", "ps_3_0"), h = Compile(material,"Hair","ps_3_0"), q = Compile(material, "Other", "ps_3_0");
        if (v.empty() || p.empty() || h.empty() || q.empty()) return false;
        kSimReceiverPs[0] = {p.size() * 4, ShaderHash(p.data(), p.size() * 4)};
        kSimReceiverPs[1] = {h.size() * 4, ShaderHash(h.data(),h.size()*4)};
        kSimHairReceiverPs[0] = kSimReceiverPs[1];
        const D3DVERTEXELEMENT9 elements[] = {{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,16,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
        return SUCCEEDED(dev->CreateVertexDeclaration(elements, &decl)) && SUCCEEDED(dev->CreateVertexShader(v.data(), &vs)) &&
               SUCCEEDED(dev->CreatePixelShader(p.data(), &body)) && SUCCEEDED(dev->CreatePixelShader(h.data(),&hair)) && SUCCEEDED(dev->CreatePixelShader(q.data(), &other)) &&
               SUCCEEDED(dev->CreateVertexBuffer(sizeof(V)*6, 0, 0, D3DPOOL_MANAGED, &vertices, nullptr)) &&
               SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
               SUCCEEDED(dev->CreateDepthStencilSurface(16,16,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,FALSE,&sceneDepth,nullptr));
    }
    void Reset(float z = .6f, float alpha = 1) {
        ReleaseSimMask(); g = {}; failNextPixelSet = false; simRenderThread.store(GetCurrentThreadId());
        Check(SUCCEEDED(dev->CreateTexture(16,16,1,D3DUSAGE_RENDERTARGET,D3DFMT_G32R32F,D3DPOOL_DEFAULT,&simMask.texture,nullptr)) &&
              SUCCEEDED(simMask.texture->GetSurfaceLevel(0,&simMask.surface)), "opaque mask allocation");
        Check(SUCCEEDED(dev->CreateTexture(16,16,1,D3DUSAGE_RENDERTARGET,D3DFMT_G32R32F,D3DPOOL_DEFAULT,&simMask.hairTexture,nullptr)) &&
              SUCCEEDED(simMask.hairTexture->GetSurfaceLevel(0,&simMask.hairSurface)), "alpha mask allocation");
        dev->SetRenderTarget(0,bb); dev->SetDepthStencilSurface(sceneDepth);
        const D3DVIEWPORT9 viewport{1,1,14,14,0,1}; dev->SetViewport(&viewport);
        dev->SetVertexDeclaration(decl); dev->SetVertexShader(vs); dev->SetPixelShader(body);
        dev->SetStreamSource(0,vertices,0,sizeof(V));
        dev->SetRenderState(D3DRS_ZENABLE,TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS); dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE); dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ZERO);
        dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE); dev->SetRenderState(D3DRS_FOGENABLE,FALSE);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE); dev->SetRenderState(D3DRS_COLORWRITEENABLE,7);
        dev->SetRenderState(D3DRS_STENCILENABLE,FALSE); dev->SetRenderState(D3DRS_STENCILWRITEMASK,0x7F);
        const RECT scissor{2,2,14,14}; dev->SetScissorRect(&scissor); dev->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE);
        dev->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0,1,0);
        SetVertices(z, alpha);
    }
    void SetVertices(float z, float alpha, bool green = false) {
        overlap = false;
        const V quad[] = {{{-1,1,z,1},{green?0.0f:1.0f,green?1.0f:0.0f,0,alpha}},
                          {{1,1,z,1},{green?0.0f:1.0f,green?1.0f:0.0f,0,alpha}},
                          {{-1,-1,z,1},{green?0.0f:1.0f,green?1.0f:0.0f,0,alpha}},
                          {{1,-1,z,1},{green?0.0f:1.0f,green?1.0f:0.0f,0,alpha}}};
        void* data = nullptr; vertices->Lock(0,sizeof(quad),&data,0); std::memcpy(data,quad,sizeof(quad)); vertices->Unlock();
    }
    void SetOverlap(bool nearFirst) {
        overlap = true;
        V triangles[6]{};
        for (int layer = 0; layer < 2; ++layer) {
            const bool isNear = (layer == 0) == nearFirst;
            const float depth = isNear ? .4f : .6f;
            const V triangle[] = {{{-1,1,depth,1},{isNear?1.0f:0.0f,isNear?0.0f:1.0f,0,1}},
                                  {{1,1,depth,1},{isNear?1.0f:0.0f,isNear?0.0f:1.0f,0,1}},
                                  {{0,-1,depth,1},{isNear?1.0f:0.0f,isNear?0.0f:1.0f,0,1}}};
            std::memcpy(triangles + layer*3,triangle,sizeof(triangle));
        }
        void* data = nullptr; vertices->Lock(0,sizeof(triangles),&data,0); std::memcpy(data,triangles,sizeof(triangles)); vertices->Unlock();
    }
    HRESULT Draw() { return dev->DrawPrimitive(overlap ? D3DPT_TRIANGLELIST : D3DPT_TRIANGLESTRIP,0,2); }
    void Record() {
        Snapshot state(dev); dev->BeginScene(); RecordSimReceiver(dev,[&] { return Draw(); }); dev->EndScene();
        Check(state.Matches(dev), "replay restores states, shaders, RT, depth, viewport and scissor rectangle");
    }
    void Original() { dev->BeginScene(); Check(SUCCEEDED(Draw()), "original draw succeeds"); dev->EndScene(); }
    void Mask(float& z, float& alpha, bool blended = false, unsigned x = 8, unsigned y = 8) {
        z = alpha = 0;
        if (!(blended ? simMask.hairCleared : simMask.cleared)) return;
        IDirect3DSurface9* cpu = nullptr;
        Check(SUCCEEDED(dev->CreateOffscreenPlainSurface(16,16,D3DFMT_G32R32F,D3DPOOL_SYSTEMMEM,&cpu,nullptr)), "mask readback allocation");
        if (!cpu) return;
        Check(SUCCEEDED(dev->GetRenderTargetData(blended?simMask.hairSurface:simMask.surface,cpu)), "mask readback");
        D3DLOCKED_RECT lock{}; cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        const auto* pixel = reinterpret_cast<const float*>(static_cast<const unsigned char*>(lock.pBits)+lock.Pitch*y)+x*2;
        z = pixel[0]; alpha = pixel[1]; cpu->UnlockRect(); SafeRelease(cpu);
    }
    void CheckMinBlending() {
        D3DCAPS9 caps{}; D3DDEVICE_CREATION_PARAMETERS creation{}; D3DDISPLAYMODE display{};
        const bool queried = SUCCEEDED(dev->GetDeviceCaps(&caps)) && SUCCEEDED(dev->GetCreationParameters(&creation)) &&
                             SUCCEEDED(d3d->GetAdapterDisplayMode(creation.AdapterOrdinal,&display));
        const HRESULT format = queried ? d3d->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,display.Format,
            D3DUSAGE_RENDERTARGET|D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,D3DRTYPE_TEXTURE,D3DFMT_G32R32F) : E_FAIL;
        std::printf("G32R32F MIN capability: BlendOp=%u, format HRESULT=0x%08lX\n", queried && (caps.PrimitiveMiscCaps&D3DPMISCCAPS_BLENDOP) ? 1u : 0u,static_cast<unsigned long>(format));
        Check(queried && (caps.PrimitiveMiscCaps&D3DPMISCCAPS_BLENDOP) && SUCCEEDED(format),"native G32R32F MIN blend capability");
        CheckProductionMinSupport(dev);
        Reset();
        dev->SetRenderTarget(0,simMask.surface); dev->SetDepthStencilSurface(nullptr);
        const D3DVIEWPORT9 viewport{0,0,16,16,0,1}; dev->SetViewport(&viewport);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE); dev->SetRenderState(D3DRS_ZENABLE,FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE); dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
        dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE); dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_MIN); dev->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xFFFFFFFF,1,0);
        dev->BeginScene();
        for (const auto values : {std::pair{.4f,1.0f},std::pair{.6f,1.0f},std::pair{1.0f,.3f}}) {
            const V quad[] = {{{-1,1,.5f,1},{values.first,values.second,0,1}},{{1,1,.5f,1},{values.first,values.second,0,1}},
                              {{-1,-1,.5f,1},{values.first,values.second,0,1}},{{1,-1,.5f,1},{values.first,values.second,0,1}}};
            Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(V))),"native MIN probe draw succeeds");
        }
        dev->EndScene(); simMask.cleared=true;
        float bodyDepth=0,hairDepth=0; Mask(bodyDepth,hairDepth);
        Check(Near(bodyDepth,.4f) && Near(hairDepth,.3f),"native MIN retains nearest depth independently for both channels");
    }
    DWORD Colour() {
        IDirect3DSurface9* cpu = nullptr;
        dev->CreateOffscreenPlainSurface(16,16,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr);
        if (!cpu || FAILED(dev->GetRenderTargetData(bb,cpu))) { SafeRelease(cpu); Check(false,"colour readback"); return 0; }
        D3DLOCKED_RECT lock{}; cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        const DWORD colour = *reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(lock.pBits)+lock.Pitch*8+8*4)&0xFFFFFF;
        cpu->UnlockRect(); SafeRelease(cpu); return colour;
    }
    ~Fixture() {
        ReleaseSimMask(); SafeRelease(vertices); SafeRelease(decl); SafeRelease(vs); SafeRelease(body); SafeRelease(hair); SafeRelease(other);
        SafeRelease(bb); SafeRelease(sceneDepth); SafeRelease(dev); SafeRelease(d3d); if (window) DestroyWindow(window);
    }
};
}

int main() {
    Fixture f; if (!f.Init()) { std::puts("Native D3D9 fixture initialization failed"); return 2; }
    f.CheckMinBlending();
    float z = 0, alpha = 0;
    bool bodyAlphaPreserved = true;
    for (const float opacity : {.35f,0.0f}) {
        f.Reset(.5f,opacity); f.dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        f.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE); f.dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
        f.dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA); f.dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
        f.Record(); f.Mask(z,alpha,true);
        bodyAlphaPreserved = (Near(z,.5f) && Near(alpha,opacity)) && bodyAlphaPreserved;
        Check(f.Colour()==0, "replay leaves original colour untouched"); f.Original();
        const unsigned red=(f.Colour()>>16)&255;
        Check(opacity==0 ? red==0 : red>=88 && red<=90, "original blended body retains source opacity");
    }
    Check(bodyAlphaPreserved, "blended body replay preserves positive depth and partial/zero alpha with hair option off");
    constexpr D3DCMPFUNC comparisons[]={D3DCMP_NEVER,D3DCMP_LESS,D3DCMP_EQUAL,D3DCMP_LESSEQUAL,
        D3DCMP_GREATER,D3DCMP_NOTEQUAL,D3DCMP_GREATEREQUAL,D3DCMP_ALWAYS};
    for (const auto comparison : comparisons) {
        f.Reset(.5f,1); f.dev->SetRenderState(D3DRS_ZFUNC,comparison); f.Record();
        const bool supported=comparison==D3DCMP_LESS || comparison==D3DCMP_LESSEQUAL;
        Check(simMask.cleared==supported && !simMask.hairCleared && simMask.draws==(supported?1u:0u),
              "opaque MIN accepts only LESS and LESS_EQUAL depth comparisons");
        if (supported) {
            f.Mask(z,alpha); Check(Near(z,.5f) && Near(alpha,1),"supported opaque comparison records the visible receiver depth");
        }
        f.Reset(.5f,.35f); f.dev->SetRenderState(D3DRS_ZFUNC,comparison); f.dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        f.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE); f.dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
        f.dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA); f.dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
        f.Record();
        Check(!simMask.cleared && simMask.hairCleared && simMask.draws==1,
              "opaque depth-mode guard leaves existing blended replay selection unchanged");
    }
    for (const bool isHair : {false,true}) for (const bool nearFirst : {false,true}) {
        f.Reset(); f.dev->SetPixelShader(isHair?f.hair:f.body);
        f.SetOverlap(nearFirst); f.Record(); f.Mask(z,alpha); f.Original();
        Check((f.Colour()&0xFFFF)==0 && (f.Colour()>>16)>=229,"one original depth-writing draw keeps nearest overlapping triangle");
        std::printf("Overlap %s, %s first: receiver RG=(%.9f, %.9f)\n",isHair?"hair":"body",nearFirst?"near":"far",z,alpha);
        Check(Near(isHair?alpha:z,.4f) && Near(isHair?z:alpha,1.0f),isHair?"opaque hair replay keeps nearest overlapping triangle":"opaque body replay keeps nearest overlapping triangle");
    }
    for (const bool hairFirst : {false,true}) {
        f.Reset(); f.dev->SetPixelShader(hairFirst?f.hair:f.body); f.SetVertices(.6f,1); f.Record(); f.Original();
        f.dev->SetPixelShader(hairFirst?f.body:f.hair); f.SetVertices(.4f,1); f.Record(); f.Original(); f.Mask(z,alpha);
        Check(Near(z,hairFirst?.4f:.6f) && Near(alpha,hairFirst?.6f:.4f),"opaque body and hair keep independent nearest depths across draws");
    }
    for (const bool isHair : {false,true}) {
        f.Reset(.5f,0); f.dev->SetPixelShader(isHair?f.hair:f.body);
        f.dev->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE); f.dev->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);
        f.dev->SetRenderState(D3DRS_ALPHAREF,128); f.Record(); f.Mask(z,alpha);
        Check(Near(z,1) && Near(alpha,1),"opaque MIN routing preserves original alpha-test rejection");
        f.Original(); Check(f.Colour()==0,"alpha-test rejected original remains invisible");
    }
    f.Reset(); f.Record(); f.Mask(z,alpha);
    Check(Near(z,.6f) && alpha==1, "fresh LESS receiver records before the original draw");
    f.Mask(z,alpha,false,1,8); Check(Near(z,1) && Near(alpha,1),"opaque replay stays inside the original scissor rectangle");
    Check(f.Colour()==0, "opaque replay leaves original colour untouched"); f.Original();
    Check(f.Colour()==0xFF0000, "original colour is restored after replay");
    f.dev->SetPixelShader(f.other); f.SetVertices(.8f,1,true); f.Original();
    Check(f.Colour()==0xFF0000, "original draw still writes depth and rejects farther geometry");

    f.Reset(); f.dev->SetPixelShader(f.other); f.SetVertices(.6f,1,true); f.Original();
    f.dev->SetPixelShader(f.body); f.SetVertices(.6f,1); f.Record(); f.Mask(z,alpha);
    Check(Near(z,1) && Near(alpha,1), "equal-depth non-Sim colour does not acquire a rejected LESS receiver");
    f.Original(); Check(f.Colour()==0x00FF00, "coplanar LESS original remains rejected");

    f.Reset(); f.dev->SetPixelShader(f.other); f.SetVertices(.3f,1,true); f.Original();
    f.dev->SetPixelShader(f.body); f.SetVertices(.6f,1); f.Record(); f.Mask(z,alpha);
    Check(Near(z,1) && Near(alpha,1), "foreground geometry rejects receiver replay");

    f.Reset(); g.p.simControls=false; f.Record();
    Check(!simMask.cleared && !simMask.hairCleared, "disabled customization skips replay");
    for (const DWORD writes : {static_cast<DWORD>(0),static_cast<DWORD>(D3DCOLORWRITEENABLE_ALPHA)}) {
        f.Reset(); f.dev->SetRenderState(D3DRS_COLORWRITEENABLE,writes); f.Record();
        Check(!simMask.cleared && !simMask.hairCleared,"colour-disabled and alpha-only draws skip receiver replay");
    }
    f.Reset(); f.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE); f.dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_SUBTRACT); f.Record();
    Check(!simMask.cleared && !simMask.hairCleared, "unsupported blend equation skips replay");
    f.Reset(); D3DVIEWPORT9 viewport{0,0,16,16,.1f,1}; f.dev->SetViewport(&viewport); f.Record();
    Check(!simMask.cleared && !simMask.hairCleared, "nonstandard depth viewport skips replay");
    f.Reset(); simMask.failed=true; f.Record();
    Check(!simMask.cleared && !simMask.hairCleared, "device-failure latch skips replay");
    f.Reset(); failNextPixelSet=true; f.Record();
    Check(simMask.failed && simMask.draws==0, "mask shader setter failure latches fallback without recording a draw");
    f.Original(); Check(f.Colour()==0xFF0000, "failed replay preserves the game's original colour draw");
    std::printf("Runtime receiver replay: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
