// Native D3D9 boundary regression check. No game hooks or game files are used.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include "d3d9_hooks.h"
#include "apex_log.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) {} }
namespace D3D9Hooks {
bool RegisterPresent(const std::string&, PresentHook, Priority) { return true; }
bool RegisterSetRenderTarget(const std::string&, SetRenderTargetHook, Priority) { return true; }
bool RegisterDrawIndexedPrimitive(const std::string&, DrawIndexedPrimitiveHook, Priority) { return true; }
bool RegisterDrawPrimitive(const std::string&, DrawPrimitiveHook, Priority) { return true; }
void UnregisterAll(const std::string&) {}
}
IDirect3DSurface9* expectedDepth = nullptr;
bool internalPass = false;
namespace DepthShare { IDirect3DSurface9* Surface() { return expectedDepth; } bool InternalPass() { return internalPass; } }
namespace ExtraHooks { HRESULT RawGetDepthStencilSurface(IDirect3DDevice9* d, IDirect3DSurface9** s) { return d->GetDepthStencilSurface(s); } }
namespace ShaderCache { bool PrecompileComplete() { return true; } }
#include "features/post_scene.cpp"
std::vector<int> applied;
void Ao(IDirect3DDevice9*) { applied.push_back(10); }
void Aa(IDirect3DDevice9*) { applied.push_back(20); }
void Blur(IDirect3DDevice9*) { applied.push_back(30); }
int checks = 0;
void Check(bool ok, const char* text) { ++checks; if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text); std::exit(1); } }
int main() {
    HWND window = CreateWindowExW(0,L"STATIC",L"Apex boundary check",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) return 2;
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
    pp.BackBufferWidth=64; pp.BackBufferHeight=64; pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D16;
    IDirect3DDevice9* dev=nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev))) return 2;
    dev->GetDepthStencilSurface(&expectedDepth);
    IDirect3DSurface9* other=nullptr;
    Check(SUCCEEDED(dev->CreateDepthStencilSurface(64,64,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&other,nullptr)),"alternate depth surface");
    g_effects={{10,Ao},{20,Aa},{30,Blur}};
    auto frame=[&](int draws) { applied.clear();dev->SetDepthStencilSurface(expectedDepth);OnFrameBoundary(dev);dev->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);for(int i=0;i<draws;++i)OnGameDraw(dev); };
    auto boundary=[&] { dev->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);OnGameDraw(dev); };
    frame(20);boundary();Check(applied==std::vector<int>({10,20,30}),"normal order");boundary();AtEndSceneBeforeOverlay(dev);Check(applied.size()==3,"normal frame runs once");
    frame(20);AtEndSceneBeforeOverlay(dev);Check(applied==std::vector<int>({10,20,30}),"hidden UI fallback order");AtEndSceneBeforeOverlay(dev);Check(applied.size()==3,"fallback runs once");
    frame(20);dev->SetDepthStencilSurface(other);boundary();Check(applied.empty()&&!g_done,"invalid depth does not consume effects");
    dev->SetDepthStencilSurface(expectedDepth);boundary();AtEndSceneBeforeOverlay(dev);Check(applied.empty(),"no retry over already drawn UI");
    dev->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);OnGameDraw(dev);boundary();Check(applied==std::vector<int>({10,20,30}),"resumed real scene accepts later boundary");
    frame(19);boundary();AtEndSceneBeforeOverlay(dev);Check(applied.empty(),"incomplete scene skipped");
    frame(20);internalPass=true;boundary();Check(applied.empty(),"internal draw ignored");internalPass=false;boundary();Check(applied.size()==3,"real boundary after internal draw");
    frame(20);OnPreReset(dev);AtEndSceneBeforeOverlay(dev);Check(applied.empty(),"reset prevents stale surfaces");
    other->Release();expectedDepth->Release();expectedDepth=nullptr;dev->Release();d3d->Release();DestroyWindow(window);
    std::printf("PASS: %d native post-scene checks\n",checks);
}
