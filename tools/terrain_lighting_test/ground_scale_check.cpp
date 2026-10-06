#define wmain terrain_suite_main
#include "check.cpp"
#undef wmain
int wmain(int argc, wchar_t** argv) {
    if(argc!=2)return 2;
    const std::filesystem::path capture=argv[1];
    for(const auto item : {std::pair{L"15-34-01 Light capture/PS_8FD18290.bin",7u},std::pair{L"15-34-05 Light capture/PS_937EA020.bin",11u}}) {
        std::ifstream stream(capture/item.first,std::ios::binary|std::ios::ate);
        Check(bool(stream),"captured shader exists"); if(!stream)continue;
        auto bytes=stream.tellg();std::vector<DWORD> code(size_t(bytes)/4);stream.seekg(0);stream.read(reinterpret_cast<char*>(code.data()),bytes);
        bool squared=false;int k=ShaderPatches::LightMapScaleConst(code,item.second,&squared);
        std::printf("Capture sampler s%u: constant c%d, squared=%d\n",item.second,k,squared);
        Check(k==(item.second==7?4:7),"captured lamp-only constant identified");
        Check(squared==(item.second==7),"captured exponent identified");
        if(item.second==7) {
            // Locate the captured scalar square without changing the capture.
            size_t squareAt=0;
            for(size_t i=1;i+3<code.size();) {
                const DWORD op=code[i]&0xffff; if(op==0xffff)break;
                if(op==0xfffe){i+=1+((code[i]>>16)&0x7fff);continue;}
                const size_t n=(code[i]>>24)&15;
                if(op==5&&n==3&&code[i+2]==code[i+3]&&(code[i+2]&0x7ff)==4)squareAt=i;
                i+=n+1;
            }
            Check(squareAt!=0,"captured square instruction found");
            if(squareAt) {
                auto bad=code;bad[squareAt+3]=(bad[squareAt+3]&~0x7ffu)|5u;
                squared=true;Check(ShaderPatches::LightMapScaleConst(bad,7,&squared)==-1&&!squared,"mixed constants are not a square");
                bad=code;bad.insert(bad.end()-1,{0x02000001u,0x80010000u,0xA0000004u});
                squared=true;Check(ShaderPatches::LightMapScaleConst(bad,7,&squared)==-1&&!squared,"shared lamp constant is refused");
                bad=code;bad[squareAt+1]|=0x00100000u;
                squared=true;Check(ShaderPatches::LightMapScaleConst(bad,7,&squared)==-1&&!squared,"saturated square is refused");
            }
        }
    }
    Gpu gpu;if(!gpu.dev)return 2;
    auto* texture=gpu.Texture(true);D3DLOCKED_RECT lock{};texture->LockRect(0,&lock,nullptr,0);
    for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=0xff202020;
    texture->UnlockRect(0);gpu.dev->SetTexture(2,texture);
    const auto linearCode=Compile("sampler2D lm:register(s2);float4 c:register(c0);float4 a:register(c1);float4 main(float2 uv:TEXCOORD0):COLOR0{float4 l=tex2D(lm,uv);return float4(l.rgb*c.x+a.rgb,l.a);}","ps_3_0");
    const auto squareCode=Compile("sampler2D lm:register(s2);float4 c:register(c0);float4 a:register(c1);float4 main(float2 uv:TEXCOORD0):COLOR0{float4 l=tex2D(lm,uv);return float4(l.rgb*(c.x*c.x)+a.rgb,l.a);}","ps_3_0");
    bool squared=false;Check(ShaderPatches::LightMapScaleConst(linearCode,2,&squared)==-1&&!squared,"direct-output synthetic layouts remain unsupported");
    Check(ShaderPatches::LightMapScaleConst(squareCode,2,&squared)==-1&&!squared,"direct-output squared synthetic layouts remain unsupported");
    auto* linear=gpu.Shader(linearCode);auto* square=gpu.Shader(squareCode);
    for(float night:{0.f,.25f,.5f,.75f,1.f})for(float gain:{.25f,.675f,.75f,1.f,1.21467388f,2.146739f,2.5f,3.f}) {
        float c[4]={TerrainLightingPolicy::LampScale(1,night,gain,false),0,0,0};gpu.dev->SetPixelShaderConstantF(0,c,1);
        const auto a=gpu.Render(linear);
        c[0]=TerrainLightingPolicy::LampScale(1,night,gain,true);gpu.dev->SetPixelShaderConstantF(0,c,1);
        const auto b=gpu.Render(square);
        for(size_t i=0;i<a.size();++i)Check(std::abs(int(a[i]&255)-int(b[i]&255))<=1,"linear and squared paths agree across brightness/phase");
    }
    gpu.dev->SetTexture(2,nullptr);Release(texture);Release(linear);Release(square);
    Policies();SamplerRestoration();
    std::printf("Ground scale: %d checks, %d failures\n",checks,failures);return failures?1:0;
}