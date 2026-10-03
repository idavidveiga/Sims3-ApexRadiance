#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include "shader_patches.h"
#include "temporal_sampling.h"
#include "resolve_under_test.h"
#include "inverse_under_test.h"
int wmain(int argc,wchar_t** argv) {
    assert(argc==2);
    namespace fs=std::filesystem;
    std::vector<DWORD> water;
    for(const auto& f: fs::directory_iterator(argv[1])) if(f.path().filename().wstring()==L"VS_2D65E890.bin") {
        std::ifstream in(f.path(),std::ios::binary);std::vector<char> b((std::istreambuf_iterator<char>(in)),{});
        water.resize(b.size()/4);memcpy(water.data(),b.data(),b.size());
    }
    assert(!water.empty());auto copy=water;
    assert(ShaderPatches::AddJitterVs(copy,252)==ShaderPatches::JitterResult::ProjectedWater && copy==water);
    copy=water;assert(ShaderPatches::AddScreenPosVs(copy,7));
    IDirect3D9* d=Direct3DCreate9(D3D_SDK_VERSION);assert(d);
    HWND w=CreateWindowExW(0,L"STATIC",L"Apex offline temporal check",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);assert(w);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=w;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    IDirect3DDevice9* dev=nullptr;
    HRESULT hr=d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,w,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);assert(SUCCEEDED(hr));
    IDirect3DVertexShader9* vs=nullptr;assert(SUCCEEDED(dev->CreateVertexShader(copy.data(),&vs)));vs->Release();
    assert(SUCCEEDED(dev->CreateVertexShader(water.data(),&vs)));vs->Release();
    ID3DBlob *shader=nullptr,*errors=nullptr;
    hr=D3DCompile(kResolveSource,strlen(kResolveSource),"Apex temporal candidate",nullptr,nullptr,"ResolvePS","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors);
    if(FAILED(hr)){if(errors)std::puts((char*)errors->GetBufferPointer());return 3;}
    IDirect3DPixelShader9* ps=nullptr;assert(SUCCEEDED(dev->CreatePixelShader((DWORD*)shader->GetBufferPointer(),&ps)));ps->Release();shader->Release();if(errors)errors->Release();
    unsigned cases=0;
    for(unsigned width:{1280u,1920u,3840u}) for(int phase:{-1,0,1}) for(double zoom:{0.6,1.0,2.4}) {
        const unsigned height=width*9/16;
        double camera[4][4]={{zoom,0,0,0.1},{0,zoom,0,-0.2},{0,0,1.00008,-0.25},{0,0,1,0}};
        double jc[4][4],inv[4][4];TemporalSampling::Jittered(camera,phase,width,height,jc);assert(Invert(jc,inv));
        for(double z:{0.26,2.0,30.0}) {
            double p[4]={0.2,0.3,z,1},a[4]{},b[4]{};
            for(int i=0;i<4;++i)for(int j=0;j<4;++j){a[i]+=camera[i][j]*p[j];b[i]+=jc[i][j]*p[j];}
            assert(a[2]==b[2] && a[3]==b[3]);
            const double x=phase<0?0:(phase==0?0.5:-0.5)/width,y=phase<0?0:(phase==0?-0.5:0.5)/height;
            assert(std::abs((b[0]/b[3]-a[0]/a[3])-x)<1e-12 && std::abs((b[1]/b[3]-a[1]/a[3])-y)<1e-12);
            double restored[4]{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)restored[i]+=inv[i][j]*b[j];
            for(int i=0;i<4;++i)assert(std::abs(restored[i]-p[i])<1e-10);
            ++cases;
        }
    }
    for(int currentPhase:{-1,0,1}) for(int previousPhase:{-1,0,1}) for(double zoom:{0.6,1.0,2.4}) {
        double cur[4][4]={{zoom,0,0,0.1},{0,zoom,0,-0.2},{0,0,1.00008,-0.25},{0,0,1,0}};
        double prev[4][4]={{1.2,0,0,-0.3},{0,1.2,0,0.1},{0,0,1.00008,-0.25},{0,0,1,0}};
        double a[4][4],b[4][4],inv[4][4],m[4][4]{};
        TemporalSampling::Jittered(cur,currentPhase,3840,2160,a);
        TemporalSampling::Jittered(prev,previousPhase,3840,2160,b);assert(Invert(a,inv));
        for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)m[i][j]+=b[i][k]*inv[k][j];
        double point[4]={0.2,0.3,3,1},now[4]{},expected[4]{},actual[4]{};
        for(int i=0;i<4;++i)for(int j=0;j<4;++j){now[i]+=a[i][j]*point[j];expected[i]+=b[i][j]*point[j];}
        for(int i=0;i<4;++i)for(int j=0;j<4;++j)actual[i]+=m[i][j]*now[j];
        for(int i=0;i<4;++i)assert(std::abs(actual[i]-expected[i])<1e-10);
        ++cases;
    }
    // A normal transformed VS still receives jitter; pool protection is not a global T2x disable.
    std::vector<DWORD> ordinary={0xfffe0200,0x0200001f,0x80000000,0x900f0000,0x03000009,0xc0010000,0x90e40000,0xa0e40000,0x03000009,0xc0020000,0x90e40000,0xa0e40001,0x03000009,0xc0040000,0x90e40000,0xa0e40002,0x03000009,0xc0080000,0x90e40000,0xa0e40003,0x0000ffff};
    assert(ShaderPatches::AddJitterVs(ordinary,252)==ShaderPatches::JitterResult::Ok);
    assert(SUCCEEDED(dev->CreateVertexShader(ordinary.data(),&vs)));vs->Release();
    dev->Release();d->Release();DestroyWindow(w);
    std::printf("Captured pool unchanged, existing grain path accepted, ordinary jitter preserved; resolve shader compiled/accepted; %u phase/inverse cases passed\n",cases);
}
