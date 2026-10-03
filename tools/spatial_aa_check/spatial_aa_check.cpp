#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include "shader_cache.h"
#include "apex_log.h"
namespace ShaderCache {
std::vector<Desc>& registry(){static std::vector<Desc> v;return v;}
Id Add(Desc d){registry().push_back(std::move(d));return int(registry().size()-1);}
Result CreatePixelShader(IDirect3DDevice9* dev,Id id,IDirect3DPixelShader9** out,std::string* error){
 auto& d=registry().at(id);std::vector<D3D_SHADER_MACRO> m;for(auto& x:d.macros)m.push_back({x.first.c_str(),x.second.c_str()});m.push_back({nullptr,nullptr});
 ID3DBlob *b=nullptr,*e=nullptr;auto hr=D3DCompile(d.source.data(),d.source.size(),d.sourceName,m.data(),nullptr,d.entry,d.target,d.flags,0,&b,&e);
 if(FAILED(hr)){if(e){if(error)*error=(char*)e->GetBufferPointer();puts((char*)e->GetBufferPointer());e->Release();}return Result::CompileFailed;}
 hr=dev->CreatePixelShader((DWORD*)b->GetBufferPointer(),out);b->Release();if(e)e->Release();return SUCCEEDED(hr)?Result::Ok:Result::CreateFailed;
}
}
namespace ApexLog { void Write(Level,const std::string& s,const std::source_location&){puts(s.c_str());} }
#include "aa_under_test.h"
namespace DepthShare { IDirect3DTexture9* Texture(){return nullptr;} }
std::vector<DWORD> Read(IDirect3DDevice9* dev){
 IDirect3DSurface9 *bb=nullptr,*rt=nullptr,*cpu=nullptr;assert(SUCCEEDED(dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb)));
 D3DSURFACE_DESC d;bb->GetDesc(&d);assert(SUCCEEDED(dev->CreateRenderTarget(d.Width,d.Height,d.Format,D3DMULTISAMPLE_NONE,0,FALSE,&rt,nullptr)));
 assert(SUCCEEDED(dev->StretchRect(bb,nullptr,rt,nullptr,D3DTEXF_NONE)));assert(SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width,d.Height,d.Format,D3DPOOL_SYSTEMMEM,&cpu,nullptr)));
 assert(SUCCEEDED(dev->GetRenderTargetData(rt,cpu)));D3DLOCKED_RECT l;assert(SUCCEEDED(cpu->LockRect(&l,nullptr,D3DLOCK_READONLY)));
 std::vector<DWORD> v(d.Width*d.Height);for(UINT y=0;y<d.Height;y++)memcpy(v.data()+y*d.Width,(char*)l.pBits+y*l.Pitch,d.Width*4);cpu->UnlockRect();cpu->Release();rt->Release();bb->Release();return v;
}
struct V {float x,y,z,w;DWORD color;};
void Scene(IDirect3DDevice9* dev,bool diagonal,DWORD background=0x7f008200){
 assert(SUCCEEDED(dev->BeginScene()));dev->SetRenderState(D3DRS_ZENABLE,FALSE);dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetRenderState(D3DRS_COLORWRITEENABLE,15);dev->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff);dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);dev->SetTexture(0,nullptr);dev->SetPixelShader(nullptr);dev->SetVertexShader(nullptr);dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
 assert(SUCCEEDED(dev->Clear(0,nullptr,D3DCLEAR_TARGET,background,1,0)));
 if(diagonal){V v[]={{12.3f,12.7f,0,1,0x7fff0000},{117.7f,109.3f,0,1,0x7fff0000},{12.3f,109.3f,0,1,0x7fff0000}};assert(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,v,sizeof(V))));}
 assert(SUCCEEDED(dev->EndScene()));
}
int main(){
 auto d=Direct3DCreate9(D3D_SDK_VERSION);assert(d);HWND w=CreateWindowExW(0,L"STATIC",L"Apex spatial AA check",WS_POPUP,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);assert(w);int modes=0;
 for(auto mode:{D3DMULTISAMPLE_NONE,D3DMULTISAMPLE_2_SAMPLES,D3DMULTISAMPLE_4_SAMPLES,D3DMULTISAMPLE_8_SAMPLES}){
 DWORD levels=0;if(FAILED(d->CheckDeviceMultiSampleType(0,D3DDEVTYPE_HAL,D3DFMT_A8R8G8B8,TRUE,mode,&levels))||FAILED(d->CheckDeviceMultiSampleType(0,D3DDEVTYPE_HAL,D3DFMT_D24S8,TRUE,mode,nullptr))){printf("MSAA %u unsupported; skipped\n",mode);continue;}
 D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferWidth=128;pp.BackBufferHeight=128;pp.hDeviceWindow=w;pp.MultiSampleType=mode;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24S8;
 IDirect3DDevice9* dev=nullptr;assert(SUCCEEDED(d->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev)));g.p=Params{};if(mode!=D3DMULTISAMPLE_NONE){assert(!InitResources(dev));assert(!g.ready);printf("Native MSAA %u correctly pauses smoothing\n",mode);ReleaseResources();dev->Release();continue;}assert(InitResources(dev));
 if(modes==0){for(size_t i=0;i<ShaderCache::registry().size();i++){IDirect3DPixelShader9* ps=nullptr;assert(ShaderCache::CreatePixelShader(dev,int(i),&ps,nullptr)==ShaderCache::Result::Ok);ps->Release();}printf("%zu production shader variants compiled and created\n",ShaderCache::registry().size());}
 if(mode==D3DMULTISAMPLE_NONE){
  g.p.smaaQuality=2;std::vector<std::pair<std::string,std::string>> original=ShaderCache::registry()[kSmaaPsId[2][0]].macros;
  unsigned edits[2]{};
  for(int color=0;color<2;color++){
   ReleaseShaders();auto& macros=ShaderCache::registry()[kSmaaPsId[2][0]].macros;macros=original;
   if(!color)macros.erase(std::remove_if(macros.begin(),macros.end(),[](auto& x){return x.first=="APEX_SMAA_COLOR_EDGES";}),macros.end());
   Scene(dev,true,0x7f004c00);auto before=Read(dev);assert(SUCCEEDED(dev->BeginScene()));static const float no[4]={};RunSmaa(dev,false,no);assert(SUCCEEDED(dev->EndScene()));auto after=Read(dev);for(size_t i=0;i<after.size();i++)if(after[i]!=before[i])edits[color]++;
  }
  assert(edits[1]>edits[0]);printf("Similar-brightness color boundary: old luma %u / new color %u pixels smoothed\n",edits[0],edits[1]);
  ReleaseShaders();ShaderCache::registry()[kSmaaPsId[2][0]].macros=original;
 }
 for(bool diagonal:{false,true})for(int q=0;q<kSmaaLevels;q++){
  Scene(dev,diagonal);auto before=Read(dev);g.p.smaaQuality=q;IDirect3DSurface9 *ds=nullptr,*rt=nullptr;dev->GetDepthStencilSurface(&ds);dev->GetRenderTarget(0,&rt);dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,FALSE);dev->SetRenderState(D3DRS_MULTISAMPLEMASK,0x55555555);
  assert(SUCCEEDED(dev->BeginScene()));static const float no[4]={};auto count=g.framesSmoothed;RunSmaa(dev,false,no);assert(SUCCEEDED(dev->EndScene()));assert(g.framesSmoothed==count+1&&!g.resolveFailed);
  IDirect3DSurface9 *afterDs=nullptr,*afterRt=nullptr;dev->GetDepthStencilSurface(&afterDs);dev->GetRenderTarget(0,&afterRt);assert(afterDs==ds&&afterRt==rt);DWORD state;dev->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS,&state);assert(state==FALSE);dev->GetRenderState(D3DRS_MULTISAMPLEMASK,&state);assert(state==0x55555555);ds->Release();rt->Release();afterDs->Release();afterRt->Release();
  auto after=Read(dev);int changed=0;for(size_t i=0;i<after.size();i++){assert((after[i]&0xff000000)==(before[i]&0xff000000));if(after[i]!=before[i])changed++;}if(!diagonal)assert(changed==0);printf("MSAA %u preset %d %s: %d changed pixels; alpha/DS/RT/mask restored\n",mode,q,diagonal?"edge":"flat",changed);
 }
 ReleaseResources();ReleaseShaders();dev->Release();modes++;
 }
 d->Release();DestroyWindow(w);assert(modes>=1);printf("PASS: %d supported sampling modes, spatial rendering/state checks\n",modes);
}

