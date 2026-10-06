// Read-only harness: production policies and patches, captured shaders and D3D9
// pixel readback. Prints to stdout; never writes into the game or capture folder.
#define NOMINMAX
#include "features/terrain_lighting_policy.h"
#include "features/native_terrain_sampler.h"
#include "features/shader_patches.h"
#include <d3dcompiler.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; if (failures < 30) std::printf("FAIL: %s\n", message); }
}
bool Near(float a, float b) { return std::abs(a - b) <= 2e-5f * std::max(1.f, std::abs(b)); }
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
std::vector<DWORD> Compile(const std::string& source, const char* profile) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", profile,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) std::printf("%s\n", static_cast<char*>(errors->GetBufferPointer()));
    Check(SUCCEEDED(hr), "HLSL compiles");
    std::vector<DWORD> result;
    if (code) result.assign(static_cast<DWORD*>(code->GetBufferPointer()), static_cast<DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code); Release(errors);
    return result;
}
void Policies() {
    using namespace TerrainLightingPolicy;
    for(int n=0;n<=100;++n)for(int g=0;g<=80;++g){
        const float night=n/100.f,gain=g/10.f,effective=SurfaceLampGain(night,gain);
        Check(std::isfinite(effective)&&effective>=0,"surface gain finite and nonnegative");
        Check(effective<=gain&&effective>=std::min(gain,1.f)*.08f,"surface gain stays within daytime and saved nighttime bounds");
        if(n==0)Check(effective==std::min(gain,1.f)*.08f,"daytime surfaces use subdued lamp response");
        if(n==100)Check(effective==gain,"nighttime surface gain stays bit exact");
        if(gain<=1)Check(effective<=gain,"surface strengths below one are never increased");
    }
    for (int n = 0; n <= 100; ++n)
        for (int g = 1; g <= 120; ++g)
            for (int v = 0; v <= 20; ++v) {
                const float night = n / 100.f, gain = g / 40.f, native = v / 20.f;
                const float single = LampScale(native, night, gain, false), multi = LampScale(native, night, gain, true);
                Check(single >= 0 && std::isfinite(single), "finite single-pass scale");
                Check(multi >= 0 && std::isfinite(multi), "finite squared scale");
                if (n == 100) {
                    const float previousWeighted = 1.f + (gain - 1.f) * night;
                    Check(single == native * previousWeighted, "exact previous night scale");
                    Check(multi == native * std::sqrt(previousWeighted), "exact previous squared night scale");
                }
                if (v == 0 && n == 0) {
                    Check(Near(single, gain), "captured c7.x=0 gets lamp light during day");
                    Check(Near(multi * multi, gain), "captured c3.x=0 gets equivalent lamp light during day");
                }
            }
    for (unsigned bits = 0; bits < 32; ++bits) {
        const bool night = bits & 1, automatic = bits & 2, user = bits & 4, force = bits & 8, world = bits & 16;
        Check(DeferDayEdit(night, automatic, user, force, world) == (!night && automatic && !user && !force && !world), "edit priority truth table");
    }
    Check(!DeferDayEdit(false, true, true, false, false), "regression: daytime lot placement must not be discarded");
    Check(!DeferDayEdit(false, true, false, true, false), "regression: daytime feature toggle must not be discarded");
    Check(LampScale(-1, 0, .75f, false) == -1, "unknown negative native scale unchanged");
    Check(std::isnan(LampScale(std::numeric_limits<float>::quiet_NaN(), 0, 1, true)), "unknown NaN scale unchanged");
    for (bool squared : {false, true}) {
        Check(LampScale(0, 0, 0, squared) == 0, "zero gain emits no daytime lamps");
        Check(LampScale(1, 1, 0, squared) == 0, "zero gain emits no night lamps");
        Check(LampScale(1, 0, -1, squared) == 1, "invalid negative gain leaves native shader scale");
        Check(LampScale(1, 0, std::numeric_limits<float>::infinity(), squared) == 1, "infinite gain leaves native shader scale");
    }
    Check(DayLampScale(0, std::numeric_limits<float>::quiet_NaN()) == 0, "invalid lot daylight gain cannot inject NaN");
    Check(DayLampScale(std::numeric_limits<float>::quiet_NaN(), 1) == 0, "invalid lot daylight level cannot inject NaN");
    Cycle c; Phase output;
    c.Reset(0); c.Observe(1, 0, 2000, true, false);
    Check(c.pending && !c.Consume(1999, output), "dusk debounce");
    Check(c.Consume(2000, output) && output == Phase::Night, "dusk endpoint consumed");
    Check(!c.Consume(2001, output), "dusk is consumed once");
    c.Observe(0, 2100, 2000, true, false);
    Check(c.Consume(4100, output) && output == Phase::Day, "regression: return to day reconciles maps");
    c.Observe(1, 4200, 2000, true, false); c.Observe(.5f, 4300, 2000, true, false);
    Check(!c.pending && !c.Consume(10000, output), "reversal cancels stale night rebuild");
    c.Observe(0, 10000, 2000, true, false); c.Observe(0, 10001, 2000, true, true);
    Check(!c.pending, "load merges phase rebuild");
    c.Observe(1, 10002, 0, false, false); Check(!c.pending, "disabled automatic rebuild remains disabled");
    Check(PhaseDelay(2000, false) == 2000, "Live mode retains saved phase delay");
    Check(PhaseDelay(2000, true) == 0, "Build preview removes the captured two-second wait");
    Check(PhaseDelay(-1, false) == 0, "negative configured delay is bounded");
    for (size_t chunks = 0; chunks <= 256; ++chunks) {
        Check(PreviewPriorityChunks(chunks, true, true) == std::min<size_t>(chunks, 4), "preview priority is bounded to four chunks");
        Check(PreviewPriorityChunks(chunks, false, true) == 0, "Live sweep receives no preview priority");
        Check(PreviewPriorityChunks(chunks, true, false) == 0, "unknown camera receives no arbitrary preview priority");
    }
    // Replay the actual captured endpoint sequence with its timestamps. Old
    // production held each endpoint for 2s; Build scheduling now consumes it in
    // the same observation, without speeding up the game's native TOD fade.
    c.Reset(1);
    c.Observe(.8f, 35782, PhaseDelay(2000, true), true, false);
    Check(!c.pending, "captured twilight does not request an intermediate rebuild");
    c.Observe(0, 36438, PhaseDelay(2000, true), true, false);
    Check(c.Consume(36438, output) && output == Phase::Day, "captured daylight endpoint schedules in the same frame");
    c.Observe(.2f, 39954, PhaseDelay(2000, true), true, false);
    c.Observe(1, 40501, PhaseDelay(2000, true), true, false);
    Check(c.Consume(40501, output) && output == Phase::Night, "captured night reversal schedules in the same frame");
    c.Reset(0);
    c.Observe(1, 10, PhaseDelay(2000, true), true, true);
    Check(!c.pending, "immediate Build scheduling still merges into world load");
    c.Reset(0);
    std::mt19937 rng(713);
    for (int64_t frame = 0; frame < 200000; ++frame) {
        const float level = (rng() % 3) * .5f;
        c.Observe(level, frame * 16, 100, true, false);
        if (c.Consume(frame * 16, output)) {
            Check(output == LevelPhase(level), "stress: only current endpoint may rebuild");
            Check(!c.Consume(frame * 16, output), "stress: repeated frames cannot duplicate work");
        }
        Check(!c.pending || c.target == LevelPhase(level), "stress: no stale target");
    }
}

struct MockDevice {
    DWORD states[16][11]{};
    IDirect3DBaseTexture9* textures[16]{};
    int reads = 0, failRead = -1, writes = 0, failWrite = -1, textureWrites = 0;
    bool failTextureRead = false, failTextureWrite = false;
    HRESULT GetTexture(DWORD s, IDirect3DBaseTexture9** p) {
        if (failTextureRead) return E_FAIL;
        *p = textures[s]; return S_OK;
    }
    HRESULT GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* p) {
        if (reads++ == failRead) return E_FAIL;
        *p = states[s][t - 1]; return S_OK;
    }
    HRESULT SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
        if (writes++ == failWrite) return E_FAIL;
        states[s][t - 1] = v; return S_OK;
    }
};
HRESULT MockSet(MockDevice* d, DWORD s, IDirect3DBaseTexture9* p) {
    ++d->textureWrites;
    if (d->failTextureWrite) return E_FAIL;
    d->textures[s] = p; return S_OK;
}
void SamplerRestoration() {
    for (int failure = -1; failure < 33; ++failure) {
        MockDevice d;
        for (int i = 0; i < 11; ++i) { d.states[2][i] = DWORD(i + 10); d.states[3][i] = DWORD(i + 50); }
        std::array<DWORD, 11> before; std::memcpy(before.data(), d.states[3], sizeof d.states[3]);
        if (failure < 22) d.failRead = failure; else d.failWrite = failure - 22;
        auto* native = reinterpret_cast<IDirect3DBaseTexture9*>(uintptr_t(1));
        {
            NativeTerrainSampler guard(&d, 2, 3, native, MockSet);
            Check(bool(guard) == (failure == -1), "sampler partial failures fail closed");
            if (guard) Check(d.textures[3] == native && !std::memcmp(d.states[2], d.states[3], sizeof d.states[2]), "exact native sampler alias");
        }
        Check(!std::memcmp(before.data(), d.states[3], sizeof d.states[3]), "every sampler state restored after success/failure");
        Check(d.textures[3] == nullptr, "texture binding restored");
    }
    MockDevice d; auto* native = reinterpret_cast<IDirect3DBaseTexture9*>(uintptr_t(1));
    { NativeTerrainSampler guard(&d, 2, 3, native, MockSet); Check(bool(guard), "same sampler states accepted"); }
    Check(d.writes == 0 && d.textureWrites == 2, "unchanged sampler states generate no writes");
    for (bool getFailure : {false, true}) {
        MockDevice failed;
        for (int i = 0; i < 11; ++i) { failed.states[2][i] = DWORD(i + 10); failed.states[3][i] = DWORD(i + 50); }
        std::array<DWORD, 11> before; std::memcpy(before.data(), failed.states[3], sizeof failed.states[3]);
        failed.failTextureRead = getFailure; failed.failTextureWrite = !getFailure;
        { NativeTerrainSampler guard(&failed, 2, 3, native, MockSet); Check(!guard, "texture Get/Set failure rejects native alias"); }
        Check(!std::memcmp(before.data(), failed.states[3], sizeof failed.states[3]) && failed.textures[3] == nullptr,
              "texture failure restores partial sampler changes without changing texture");
        Check(getFailure ? failed.writes == 0 && failed.textureWrites == 0 : failed.textureWrites == 1,
              "failed texture operation performs no extra texture writes");
    }
}

struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex terrain checks", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{}; p.Windowed = TRUE; p.SwapEffect = D3DSWAPEFFECT_DISCARD; p.hDeviceWindow = window;
        p.BackBufferFormat = D3DFMT_UNKNOWN; p.BackBufferWidth = 64; p.BackBufferHeight = 64;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
        Check(dev != nullptr, "native D3D9 device available (required GPU validation)");
        if (!dev) return;
        D3DADAPTER_IDENTIFIER9 id{}; api->GetAdapterIdentifier(0, 0, &id); std::printf("D3D9 adapter: %s\n", id.Description);
        Check(SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)), "readback target");
        Check(SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)), "readback surface");
        std::printf("GPU targets ready\n");
        dev->SetRenderTarget(0, target); dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_DITHERENABLE, FALSE); dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        std::printf("GPU draw state ready\n");
    }
    ~Gpu() { Release(target); Release(readback); Release(dev); Release(api); if (window) DestroyWindow(window); }
    IDirect3DTexture9* Texture(bool smooth) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
        D3DLOCKED_RECT r{}; t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = smooth ? 0xDB336699u : ((DWORD(20 + x * 3) << 24) | 0xAABBCCu);
        t->UnlockRect(0); return t;
    }
    IDirect3DPixelShader9* Shader(const std::vector<DWORD>& code) {
        IDirect3DPixelShader9* s = nullptr;
        Check(!code.empty() && SUCCEEDED(dev->CreatePixelShader(code.data(), &s)), "D3D9 accepts shader bytecode"); return s;
    }
    std::vector<DWORD> Render(IDirect3DPixelShader9* shader, IDirect3DVertexShader9* vs = nullptr) {
        struct Vertex { float x, y, z, rhw, u, v; };
        Vertex q[4] = {{-.5f, -.5f, 0, 1, 0, 0}, {63.5f, -.5f, 0, 1, 1, 0}, {-.5f, 63.5f, 0, 1, 0, 1}, {63.5f, 63.5f, 0, 1, 1, 1}};
        IDirect3DVertexDeclaration9* decl = nullptr;
        if (vs) {
            q[0].x = q[2].x = -1; q[1].x = q[3].x = 1; q[0].y = q[1].y = 1; q[2].y = q[3].y = -1;
            const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                                {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
            Check(SUCCEEDED(dev->CreateVertexDeclaration(elements, &decl)), "test vertex layout"); dev->SetVertexDeclaration(decl);
        } else dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetVertexShader(vs); dev->SetPixelShader(shader);
        dev->BeginScene(); Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex))), "test quad draw"); dev->EndScene();
        Check(SUCCEEDED(dev->GetRenderTargetData(target, readback)), "GPU readback");
        std::vector<DWORD> pixels(4096); D3DLOCKED_RECT r{};
        if (SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < 64; ++y) std::memcpy(pixels.data() + y * 64, static_cast<char*>(r.pBits) + y * r.Pitch, 256);
            readback->UnlockRect();
        } else Check(false, "readback lock");
        Release(decl); return pixels;
    }
};

void Patches(Gpu& gpu, const std::filesystem::path& captures) {
    for (const char* profile : {"ps_2_0", "ps_3_0"}) {
        std::printf("Testing profile %s\n", profile);
        auto code = Compile("sampler2D light:register(s2); float4 gain:register(c0); float4 main(float2 uv:TEXCOORD0):COLOR0{float4 v=tex2D(light,uv);return float4(v.rgb*gain.x,v.a);}", profile);
        if (code.empty()) continue;
        const auto original = code;
        DWORD spare = 999;
        const bool matched = ShaderPatches::PatchTerrainNativeAlpha(code, 2, spare);
        Check(matched, "SM2/SM3 native alpha patch");
        if (!matched) continue;
        Check(spare == 3, "first free sampler used");
        auto bad = original; bad.pop_back(); const auto saved = bad;
        Check(!ShaderPatches::PatchTerrainNativeAlpha(bad, 2, spare) && bad == saved, "truncated shader untouched");
        for (size_t size = 0; size < original.size(); ++size) {
            bad.assign(original.begin(), original.begin() + size); const auto before = bad;
            Check(!ShaderPatches::PatchTerrainNativeAlpha(bad, 2, spare) && bad == before, "every truncated instruction fails closed");
        }
        bad = original; Check(!ShaderPatches::PatchTerrainNativeAlpha(bad, 15, spare) && bad == original, "missing sampler unchanged");
        auto refuse = [&](std::vector<DWORD> candidate, const char* why) {
            const auto before = candidate; DWORD unchanged = 999;
            Check(!ShaderPatches::PatchTerrainNativeAlpha(candidate, 2, unchanged) && candidate == before && unchanged == 999, why);
        };
        bad = original; bad[0] = 0xFFFE0300; refuse(bad, "vertex shader never patched");
        bad = original; bad[0] = 0xFFFF0101; refuse(bad, "SM1 never patched");
        for (size_t i = 1; i < original.size();) {
            const DWORD tok = original[i];
            if (tok == 0xFFFF) break;
            if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
            const size_t len = (tok >> 24) & 15;
            if ((tok & 0xFFFF) == 0x42 && len == 3) {
                bad = original; bad[i + 1] = (bad[i + 1] & ~0xF0000u) | 0x70000u; refuse(bad, "partial light fetch rejected");
                bad = original; bad[i + 2] |= 0x2000; refuse(bad, "relative UV addressing rejected");
                bad = original; bad[i] |= 0x10000; refuse(bad, "projected texture fetch rejected");
                bad = original; bad[i + 1] = (bad[i + 1] & ~0x7FFu) | (std::string(profile) == "ps_2_0" ? 11u : 31u); refuse(bad, "exhausted temporary registers rejected");
                bad = original; bad.insert(bad.begin() + i, original.begin() + i, original.begin() + i + 4); refuse(bad, "ambiguous repeated light-map fetch rejected");
                break;
            }
            i += 1 + len;
        }
        if (!gpu.dev) continue;
        auto* base = gpu.Shader(original); auto* patched = gpu.Shader(code);
        const float gain[4] = {1, 0, 0, 0}; gpu.dev->SetPixelShaderConstantF(0, gain, 1);
        auto* native = gpu.Texture(false); auto* smooth = gpu.Texture(true);
        std::printf("GPU shader and textures ready\n");
        gpu.dev->SetTexture(2, native);
        for (DWORD state = 1; state <= 11; ++state) {
            // Native filters differ from the smooth sampler; the alias must clone them.
            if (state == D3DSAMP_MINFILTER || state == D3DSAMP_MAGFILTER) gpu.dev->SetSamplerState(2, static_cast<D3DSAMPLERSTATETYPE>(state), D3DTEXF_POINT);
        }
        gpu.dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        const auto nativePixels = gpu.Render(base);
        gpu.dev->SetTexture(2, smooth); const auto smoothPixels = gpu.Render(base); gpu.dev->SetTexture(2, native);
        gpu.dev->SetTexture(spare, smooth); // exercise a real previous texture reference, not just an empty spare slot
        {
            NativeTerrainSampler guard(gpu.dev, 2, spare, native, [](auto* d, DWORD slot, auto* t) { return d->SetTexture(slot, t); });
            Check(bool(guard), "real device native sampler alias"); gpu.dev->SetTexture(2, smooth);
            const auto result = gpu.Render(patched);
            for (size_t p = 0; p < result.size(); ++p) {
                Check((result[p] & 0xFFFFFFu) == (smoothPixels[p] & 0xFFFFFFu), "lamp RGB unchanged by native-alpha patch");
                Check((result[p] & 0xFF000000u) == (nativePixels[p] & 0xFF000000u), "solar alpha equals original at every pixel");
            }
            gpu.dev->SetTexture(2, native);
        }
        IDirect3DBaseTexture9* restored = nullptr;
        Check(SUCCEEDED(gpu.dev->GetTexture(spare, &restored)) && restored == smooth, "real previous spare texture binding restored");
        Release(restored);
        // CPU guard cost with the real driver, no game hooks. The render tests
        // above provide correctness; this small benchmark cannot predict FPS.
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 5000; ++i) {
            NativeTerrainSampler guard(gpu.dev, 2, spare, native, [](auto* d, DWORD slot, auto* t) { return d->SetTexture(slot, t); });
            if (!guard) { Check(false, "benchmark sampler alias"); break; }
        }
        const double micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 5000;
        std::printf("Native alpha sampler guard (%s): %.3f us/call, real-driver CPU only; one extra shader texture read\n", profile, micros);
        gpu.dev->SetTexture(2, nullptr); gpu.dev->SetTexture(spare, nullptr);
        Release(base); Release(patched); Release(native); Release(smooth);
    }
    // Validate the actual world single/multi-pass shaders from the user's captures.
    int found = 0;
    if (std::filesystem::exists(captures)) for (const auto& e : std::filesystem::recursive_directory_iterator(captures)) {
        const std::string name = e.path().filename().string();
        DWORD sampler;
        if (name == "PS_2BA33118.bin") sampler = 7;
        else if (name == "PS_2BA62940.bin") sampler = 2;
        else continue;
        std::ifstream f(e.path(), std::ios::binary); std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
        Check(raw.size() % 4 == 0 && raw.size() >= 8, "capture bytecode size");
        std::vector<DWORD> code(raw.size() / 4); std::memcpy(code.data(), raw.data(), raw.size());
        DWORD spare = 999; const auto original = code;
        Check(ShaderPatches::PatchTerrainNativeAlpha(code, sampler, spare), "captured terrain shader matches native alpha patch");
        if (name == "PS_2BA33118.bin") Check(ShaderPatches::LightMapScaleConst(original, sampler) == 7, "captured exclusive lamp constant c7");
        ID3DBlob* dis = nullptr;
        Check(SUCCEEDED(D3DDisassemble(code.data(), code.size() * 4, 0, nullptr, &dis)), "captured patched bytecode disassembles"); Release(dis);
        if (gpu.dev) { auto* shader = gpu.Shader(code); Release(shader); }
        std::printf("Captured shader %s: %zu -> %zu tokens; native alpha sampler s%lu\n", name.c_str(), original.size(), code.size(), spare);
        ++found;
    }
    Check(found >= 2, "both captured terrain variants tested");
}

std::string LotHlsl(const std::filesystem::path& source) {
    std::ifstream f(source); const std::string text((std::istreambuf_iterator<char>(f)), {});
    const auto at = text.find("const char* kReplacementHlsl = R\"(");
    if (at == std::string::npos) { Check(false, "production lot shader source found"); return {}; }
    const auto start = text.find('\n', at), end = text.find(")\";", start);
    if (end == std::string::npos) { Check(false, "production lot shader delimiter"); return {}; }
    return text.substr(start, end - start);
}
void LotPixels(Gpu& gpu, const std::filesystem::path& current, const std::filesystem::path& reference) {
    auto oldCode = Compile(LotHlsl(reference), "ps_3_0"), newCode = Compile(LotHlsl(current), "ps_3_0");
    if (!gpu.dev || oldCode.empty() || newCode.empty()) return;
    auto* oldPs = gpu.Shader(oldCode); auto* newPs = gpu.Shader(newCode);
    const auto vertex = Compile(R"(
struct Out {float4 pos:POSITION; float4 shadow:TEXCOORD2; float3 normal:TEXCOORD4; float2 lot:TEXCOORD5; float3 terrain:TEXCOORD1;};
Out main(float4 pos:POSITION,float2 uv:TEXCOORD0){Out o;o.pos=pos;o.shadow=float4(.5,.5,0,1);o.normal=float3(0,1,0);o.lot=uv;o.terrain=float3(uv,0);return o;}
)", "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr; Check(!vertex.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vertex.data(), &vs)), "lot test vertex shader");
    auto* terrain = gpu.Texture(true); auto* lot = gpu.Texture(false); auto* shadow = gpu.Texture(false);
    gpu.dev->SetTexture(1, lot); gpu.dev->SetTexture(2, terrain); gpu.dev->SetTexture(5, shadow);
    for (DWORD slot : {1ul, 2ul, 5ul}) {
        gpu.dev->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_POINT); gpu.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        gpu.dev->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    const float c0[4] = {.1f, .2f, .3f, 0}, c1[4] = {0, 1, 0, 0}, zero[4]{};
    const float x[4] = {1, 0, 0, 1}, z[4] = {0, 1, 0, 1};
    gpu.dev->SetPixelShaderConstantF(0, c0, 1); gpu.dev->SetPixelShaderConstantF(1, c1, 1);
    gpu.dev->SetPixelShaderConstantF(2, zero, 1); gpu.dev->SetPixelShaderConstantF(4, zero, 1);
    gpu.dev->SetPixelShaderConstantF(28, x, 1); gpu.dev->SetPixelShaderConstantF(29, z, 1);
    for (float scale : {0.f, .125f, .5f, 1.f, 2.f}) for (float gain : {.25f, .75f, 1.f, 2.f}) for (int feather = 0; feather < 3; ++feather) {
        const float c3[4] = {scale, 0, 0, 0}, c31[4] = {gain, 0, 0, 0};
        const float edge[4] = {feather == 2 ? 3.f : 0.f, feather == 1 ? 1.f : 0.f, 0, 0};
        gpu.dev->SetPixelShaderConstantF(3, c3, 1); gpu.dev->SetPixelShaderConstantF(30, edge, 1); gpu.dev->SetPixelShaderConstantF(31, c31, 1);
        const auto before = gpu.Render(oldPs, vs), after = gpu.Render(newPs, vs);
        for (size_t p = 0; p < before.size(); ++p) for (int channel = 0; channel < 3; ++channel)
            Check(std::abs(int((before[p] >> (channel * 8)) & 255) - int((after[p] >> (channel * 8)) & 255)) <= 1, "actual lot HLSL preserves night pixels (one LSB tolerance)");
    }
    // At day, native c3 remains zero: the window/lot map must stay native, while
    // the added terrain lamp contribution must be independent of lot-edge weight.
    const float c3[4] = {0, 0, 0, 0}, c31[4] = {1, .75f, 0, 0};
    gpu.dev->SetPixelShaderConstantF(3, c3, 1); gpu.dev->SetPixelShaderConstantF(31, c31, 1);
    std::vector<DWORD> boundary;
    for (float weight : {0.f, 1.f}) {
        const float edge[4] = {0, weight, 0, 0}; gpu.dev->SetPixelShaderConstantF(30, edge, 1);
        const auto pixels = gpu.Render(newPs, vs);
        if (boundary.empty()) boundary = pixels; else Check(boundary == pixels, "daylight lamp term continuous across lot border");
        const auto old = gpu.Render(oldPs, vs);
        Check(pixels[2000] != old[2000], "daylight terrain lamps no longer multiplied by zero");
    }
    for (DWORD slot : {1ul, 2ul, 5ul}) gpu.dev->SetTexture(slot, nullptr);
    gpu.dev->SetVertexShader(nullptr); gpu.dev->SetPixelShader(nullptr);
    Release(oldPs); Release(newPs); Release(vs); Release(terrain); Release(lot); Release(shadow);
    std::printf("Production lot HLSL: night equivalence and daytime border continuity checked\n");
}

void WallPixels(Gpu& gpu, const std::filesystem::path& capture) {
    if (!gpu.dev) return;
    std::ifstream file(capture / "PS_265EBE18.bin", std::ios::binary | std::ios::ate);
    Check(bool(file), "actual daytime wall F7 shader available");
    if (!file) return;
    const auto size = file.tellg();
    Check(size == 1372, "captured ExteriorWall shader size");
    if (size != 1372) return;
    std::vector<DWORD> code(static_cast<size_t>(size) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), size);
    DWORD hash = 2166136261u; for (DWORD word : code) hash = (hash ^ word) * 16777619u;
    Check(hash == 0x04956FE9u, "actual wall matches recognized ExteriorWall_PS_1119, c3");
    if (hash != 0x04956FE9u) return;
    auto* ps = gpu.Shader(code);
    const auto vertex = Compile(R"(
struct Out {float4 pos:POSITION;float2 uv:TEXCOORD0;float4 map:TEXCOORD1;float4 normal:TEXCOORD2;
float4 mask:TEXCOORD3;float3 eye:TEXCOORD5;float3 tangent:TEXCOORD6;float4 shadow:TEXCOORD7;float4 a:COLOR0;float4 b:COLOR1;};
Out main(float4 pos:POSITION,float2 uv:TEXCOORD0){Out o;o.pos=pos;o.uv=uv;o.map=float4(uv,uv);
o.normal=float4(0,1,0,1);o.mask=float4(.5,.5,1,0);o.eye=float3(0,1,0);o.tangent=float3(1,0,0);
o.shadow=float4(.5,.5,0,1);o.a=float4(0,0,0,1);o.b=0;return o;}
)", "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    Check(!vertex.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vertex.data(), &vs)), "wall fixture vertex shader");
    IDirect3DCubeTexture9* cube = nullptr;
    Check(SUCCEEDED(gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr)), "wall black sky cube");
    if (!ps || !vs || !cube) { Release(ps);Release(vs);Release(cube);return; }
    for(int face=0;face<6;++face){D3DLOCKED_RECT r{};cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&r,nullptr,0);
        *static_cast<DWORD*>(r.pBits)=0xff000000u;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    auto* white=gpu.Texture(true);auto* lamp=gpu.Texture(true);auto* normal=gpu.Texture(true);
    auto fill=[](IDirect3DTexture9* texture,DWORD value){D3DLOCKED_RECT r{};texture->LockRect(0,&r,nullptr,0);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+y*r.Pitch)[x]=value;texture->UnlockRect(0);};
    fill(white,0xffffffffu);fill(normal,0x80808080u);
    gpu.dev->SetTexture(0,cube);gpu.dev->SetTexture(1,cube);
    for(DWORD slot=2;slot<=8;++slot){gpu.dev->SetTexture(slot,slot==2?lamp:slot==7?normal:white);
        gpu.dev->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE);}
    float constants[11][4]{};constants[7][0]=1;constants[8][0]=1;
    gpu.dev->SetPixelShaderConstantF(0,constants[0],11);
    for(bool lit:{false,true})for(float night:{0.f,.5f,1.f})for(float gain:{1.f,2.f,4.f}){
        fill(lamp,lit?0xff201008u:0xff000000u);
        float scale[4]={night*gain,.188235313f,0,0};gpu.dev->SetPixelShaderConstantF(3,scale,1);
        const auto previous=gpu.Render(ps,vs);
        scale[0]=TerrainLightingPolicy::WallLampScale(night,night,gain);gpu.dev->SetPixelShaderConstantF(3,scale,1);
        const auto current=gpu.Render(ps,vs);
        if(!lit||night==1)Check(previous==current,"captured wall GPU: empty map and night pixels unchanged");
        const float expectedScale=night*gain+(1.f-night)*std::min(gain,1.f)*.08f;
        for(DWORD pixel:current)for(int channel=0;channel<3;++channel){const int expected=lit?int((32>>channel)*expectedScale):0;
            Check(std::abs(int((pixel>>(16-channel*8))&255)-expected)<=2,"captured wall GPU: lamp RGB survives daytime scale");}
        if(lit&&night==0)Check((previous[2000]&0xffffffu)==0&&(current[2000]&0xffffffu)!=0,"F7 regression reproduced then fixed on actual shader");
    }
    for(DWORD slot=0;slot<=8;++slot)gpu.dev->SetTexture(slot,nullptr);
    gpu.dev->SetVertexShader(nullptr);gpu.dev->SetPixelShader(nullptr);
    Release(ps);Release(vs);Release(cube);Release(white);Release(lamp);Release(normal);
    std::printf("Actual F7 wall shader: day/twilight/night RGB checks complete\n");
}

void InstancedPixels(Gpu& gpu, const std::filesystem::path& capture) {
    if(!gpu.dev)return;
    std::ifstream file(capture/"PS_3019F738.bin",std::ios::binary|std::ios::ate);
    Check(bool(file),"captured instanced bench shader available");if(!file)return;
    const auto bytes=file.tellg();Check(bytes==912,"captured instanced bench bytecode size");if(bytes!=912)return;
    std::vector<DWORD> code(static_cast<size_t>(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);
    DWORD hash=2166136261u;for(DWORD word:code)hash=(hash^word)*16777619u;
    Check(hash==0xC0F5CCD7u,"exact F7 instanced bench shader identity");if(hash!=0xC0F5CCD7u)return;
    auto* ps=gpu.Shader(code);
    const auto vertex=Compile(R"(
struct Out{float4 pos:POSITION;float4 a:COLOR0;float4 shadow:TEXCOORD0;float b:COLOR1;float4 uv:TEXCOORD1;
float3 normal:TEXCOORD2;float4 eye:TEXCOORD3;float2 spec:TEXCOORD6;};
Out main(float4 pos:POSITION,float2 uv:TEXCOORD0){Out o;o.pos=pos;o.a=0;o.shadow=float4(.5,.5,0,1);o.b=0;
o.uv=float4(uv,uv);o.normal=float3(0,1,0);o.eye=float4(0,1,0,0);o.spec=uv;return o;}
)","vs_3_0");
    IDirect3DVertexShader9* vs=nullptr;Check(!vertex.empty()&&SUCCEEDED(gpu.dev->CreateVertexShader(vertex.data(),&vs)),"instanced bench fixture VS");
    IDirect3DCubeTexture9* cube=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr)),"instanced black sky cube");
    if(!ps||!vs||!cube){Release(ps);Release(vs);Release(cube);return;}
    for(int face=0;face<6;++face){D3DLOCKED_RECT r{};cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&r,nullptr,0);
        *static_cast<DWORD*>(r.pBits)=0xff000000u;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    auto* white=gpu.Texture(true);auto* lamp=gpu.Texture(true);
    auto fill=[](IDirect3DTexture9* t,DWORD value){D3DLOCKED_RECT r{};t->LockRect(0,&r,nullptr,0);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+y*r.Pitch)[x]=value;t->UnlockRect(0);};
    fill(white,0xffffffffu);
    gpu.dev->SetTexture(0,cube);gpu.dev->SetTexture(1,cube);
    for(DWORD slot:{2ul,3ul,5ul,6ul}){gpu.dev->SetTexture(slot,slot==6?lamp:white);
        gpu.dev->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE);}
    float constants[14][4]{};constants[0][0]=constants[0][1]=constants[0][2]=.2f;constants[1][1]=1;
    constants[2][0]=constants[2][1]=constants[2][2]=1;constants[4][0]=20;constants[5][0]=1;constants[7][0]=.5f;constants[12][0]=constants[12][1]=1;
    gpu.dev->SetPixelShaderConstantF(0,constants[0],14);
    for(bool lit:{false,true})for(float night:{0.f,.5f,1.f})for(float gain:{.993083f,1.f,2.f}){
        fill(lamp,lit?0xff201008u:0xff000000u);
        float strength[4]={gain,0,0,0};gpu.dev->SetPixelShaderConstantF(13,strength,1);const auto previous=gpu.Render(ps,vs);
        strength[0]=TerrainLightingPolicy::SurfaceLampGain(night,gain);gpu.dev->SetPixelShaderConstantF(13,strength,1);const auto current=gpu.Render(ps,vs);
        if(night==1||!lit)Check(previous==current,"actual bench GPU preserves night and lamp-free sunlight pixels");
        for(DWORD pixel:current)for(int channel=0;channel<3;++channel){const int expected=51+(lit?int((32>>channel)*strength[0]):0);
            Check(std::abs(int((pixel>>(16-channel*8))&255)-expected)<=2,"actual bench GPU separates lamp attenuation from sunlight");}
        if(lit&&night==0)Check((current[2000]&0xffffffu)<(previous[2000]&0xffffffu),"actual bench daytime lamp term is reduced");
    }
    for(DWORD slot:{0ul,1ul,2ul,3ul,5ul,6ul})gpu.dev->SetTexture(slot,nullptr);
    gpu.dev->SetPixelShader(nullptr);gpu.dev->SetVertexShader(nullptr);
    Release(ps);Release(vs);Release(cube);Release(white);Release(lamp);
    std::printf("Actual F7 instanced bench: daytime lamp reduction, native sunlight and exact night pixels checked\n");
}

void TerrainCompositionPixels(Gpu& gpu, const std::filesystem::path& capture) {
    if (!gpu.dev) return;
    auto read=[](const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);std::vector<char> raw((std::istreambuf_iterator<char>(f)),{});
        std::vector<DWORD> t(raw.size()/4);if(!raw.empty())std::memcpy(t.data(),raw.data(),t.size()*4);return t;};
    auto singleCode=read(capture/"PS_4BE77B30.bin"), multiCode=read(capture/"PS_4BE76D20.bin");
    Check(singleCode.size()==387&&multiCode.size()==199,"latest F7 terrain shader sizes");
    auto* single=gpu.Shader(singleCode);auto* multi=gpu.Shader(multiCode);
    auto patchedCode=singleCode;DWORD dayConst=0;
    Check(ShaderPatches::PatchTerrainDaylightRange(patchedCode,dayConst)&&dayConst==223,"captured single matches studied light range patch");
    auto unsupported=multiCode;const auto untouched=unsupported;DWORD ignored=999;
    Check(!ShaderPatches::PatchTerrainDaylightRange(unsupported,ignored)&&unsupported==untouched&&ignored==999,"multi-pass remains untouched");
    auto malformed=singleCode;malformed[15]^=1;const auto unchanged=malformed;
    Check(!ShaderPatches::PatchTerrainDaylightRange(malformed,ignored)&&malformed==unchanged,"unstudied bytecode fails closed");
    auto* patched=gpu.Shader(patchedCode);
    if(!patched){ID3DBlob* listing=nullptr;D3DDisassemble(patchedCode.data(),patchedCode.size()*4,0,nullptr,&listing);
        if(listing){std::printf("%s\n",static_cast<char*>(listing->GetBufferPointer()));listing->Release();}}
    const auto sv=Compile(R"(
struct O{float4 p:POSITION;float4 a:TEXCOORD0;float4 b:TEXCOORD1;float2 c:TEXCOORD2;float4 d:TEXCOORD3;float3 e:TEXCOORD4;};
O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.a=float4(uv,uv);o.b=float4(uv,1,0);o.c=uv;o.d=float4(.5,.5,0,1);o.e=0;return o;}
)","vs_3_0");
    const auto mv=Compile(R"(
struct O{float4 p:POSITION;float2 a:TEXCOORD0;float3 b:TEXCOORD1;float4 c:TEXCOORD2;};
O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.a=uv;o.b=float3(uv,1);o.c=float4(.5,.5,0,1);return o;}
)","vs_3_0");
    IDirect3DVertexShader9 *singleVs=nullptr,*multiVs=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateVertexShader(sv.data(),&singleVs)),"terrain single fixture VS");
    Check(SUCCEEDED(gpu.dev->CreateVertexShader(mv.data(),&multiVs)),"terrain multi fixture VS");
    IDirect3DCubeTexture9* cube=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr)),"terrain sky cube");
    if(!single||!multi||!patched||!singleVs||!multiVs||!cube){Release(single);Release(multi);Release(patched);Release(singleVs);Release(multiVs);Release(cube);return;}
    for(int face=0;face<6;++face){D3DLOCKED_RECT r{};cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&r,nullptr,0);
        *static_cast<DWORD*>(r.pBits)=0xff000000;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    auto* normal=gpu.Texture(true);auto* material=gpu.Texture(true);auto* mask=gpu.Texture(true);
    auto* white=gpu.Texture(true);auto* detail=gpu.Texture(true);auto* lamp=gpu.Texture(true);
    auto fill=[](IDirect3DTexture9* t,DWORD v){D3DLOCKED_RECT r{};t->LockRect(0,&r,nullptr,0);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+y*r.Pitch)[x]=v;t->UnlockRect(0);};
    fill(normal,0x80808080);fill(material,0x00333333);fill(mask,0);fill(white,0xffffffff);fill(detail,0xff808080);
    for(DWORD slot=0;slot<10;++slot){gpu.dev->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT);
        gpu.dev->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE);}
    for(int rgb:{0,32,64,128,192,255}){
        fill(lamp,0xff000000u|DWORD(rgb)*0x010101u);
        float c[32][4]{};c[0][0]=c[0][1]=c[0][2]=1.35f;c[1][1]=1;c[4][1]=1;c[4][3]=2.5f;c[7][0]=1.21467388f;c[11][1]=1;c[12][2]=1;
        gpu.dev->SetPixelShaderConstantF(0,c[0],32);gpu.dev->SetTexture(0,cube);gpu.dev->SetTexture(1,normal);
        for(DWORD slot:{2ul,3ul,4ul})gpu.dev->SetTexture(slot,material);
        gpu.dev->SetTexture(5,white);gpu.dev->SetTexture(6,mask);gpu.dev->SetTexture(7,lamp);gpu.dev->SetTexture(8,detail);gpu.dev->SetTexture(9,white);
        gpu.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);const auto a=gpu.Render(single,singleVs);
        float range[4]={1,2,0,0};gpu.dev->SetPixelShaderConstantF(dayConst,range,1);const auto corrected=gpu.Render(patched,singleVs);
        range[0]=0;gpu.dev->SetPixelShaderConstantF(dayConst,range,1);const auto night=gpu.Render(patched,singleVs);
        Check(night==a,"zero daylight weight preserves every original shader pixel");
        for(float weight:{.25f,.5f,.75f}){
            range[0]=weight;gpu.dev->SetPixelShaderConstantF(dayConst,range,1);const auto twilight=gpu.Render(patched,singleVs);
            const float light=1.35f+(rgb/255.f)*1.21467388f;
            const int expected=int((light*(1-weight)+std::min(light,2.f)*weight)*.2f*255);
            Check(std::abs(int(twilight[2000]&255)-expected)<=3,"terrain range recovers continuously through twilight");
        }
        c[3][0]=1.10212243f;gpu.dev->SetPixelShaderConstantF(0,c[0],32);
        gpu.dev->SetTexture(1,normal);gpu.dev->SetTexture(2,lamp);gpu.dev->SetTexture(6,white);
        gpu.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);gpu.dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);gpu.dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
        gpu.dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff333333,1,0);const auto b=gpu.Render(multi,multiVs);
        const int av=a[2000]&255,bv=b[2000]&255;
        const float light=1.35f+(rgb/255.f)*1.21467388f;
        std::printf("Captured terrain composition: map %d, lighting %.4f, single %d, multi %d\n",rgb,light,av,bv);
        Check(std::abs(av-int(light*.2f*255))<=3,"captured single pass applies material before target clipping");
        Check(std::abs(bv-int(std::min(light,2.f)*.2f*255))<=3,"captured multi pass clips illumination before material blending");
        Check(rgb>=192 ? av>bv+5 : std::abs(av-bv)<=3,"captured two paths diverge only above light buffer range");
        for(size_t pixel=0;pixel<corrected.size();++pixel)for(int shift:{0,8,16})
            Check(std::abs(int((corrected[pixel]>>shift)&255)-int((b[pixel]>>shift)&255))<=2,"corrected single and native multi composition agree at every pixel");
        if(rgb==0)Check(corrected==a,"lamp-free solar pixels stay identical");
    }
    gpu.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    for(DWORD slot=0;slot<10;++slot)gpu.dev->SetTexture(slot,nullptr);
    gpu.dev->SetPixelShader(nullptr);gpu.dev->SetVertexShader(nullptr);
    Release(single);Release(multi);Release(patched);Release(singleVs);Release(multiVs);Release(cube);
    Release(normal);Release(material);Release(mask);Release(white);Release(detail);Release(lamp);
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto start = std::chrono::steady_clock::now();
    Policies(); std::printf("Policies: %d checks, %d failures\n", checks, failures);
    SamplerRestoration(); std::printf("Sampler restoration complete\n"); Gpu gpu;
    Patches(gpu, argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path());
    if (argc > 3 && argv[2][0] && argv[3][0]) LotPixels(gpu, argv[2], argv[3]);
    if (argc > 4 && argv[4][0]) WallPixels(gpu, argv[4]);
    if (argc > 5 && argv[5][0]) InstancedPixels(gpu, argv[5]);
    if (argc > 6 && argv[6][0]) TerrainCompositionPixels(gpu, argv[6]);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("Terrain lighting: %d checks, %d failures; harness %.1f ms (not game FPS)\n", checks, failures, ms);
    return failures ? 1 : 0;
}
