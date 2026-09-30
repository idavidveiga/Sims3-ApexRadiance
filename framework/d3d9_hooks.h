#pragma once
// Apex's D3D9 device hooks. At the first EndScene of the game's device the framework detours 15 IDirect3DDevice9
// methods; features register callbacks by name, run in priority order (lower first; equal priorities in registration
// order). A callback returning Skip or Block stops the chain and the device call (S_OK / E_FAIL is returned to the game).
// A callback may call the device (a nested dispatch on the same thread) or register and unregister callbacks; a chain
// being run keeps the list it started with until it returns.
//
// Dispatch and locking (2026-09-29, docs/architecture.md section 3.6):
//  - The draw and state chains (DrawIndexedPrimitive, DrawPrimitive, SetRenderTarget, SetViewport, SetPixelShader,
//    SetVertexShader, SetTexture, Set*ShaderConstantF) are dispatched WITHOUT a lock on the render thread (the thread of
//    the first EndScene): they read an immutable list through an atomic pointer. Registering publishes a new list under
//    the registration lock; the old lists are kept until a safe point (Present on the render thread outside every dispatch, or
//    Uninstall), so a dispatch still reading one stays valid.
//  - Present, BeginScene and the Create* chains, and every chain called from another thread, run under the one
//    recursive registration lock, as before (one such dispatch at a time).
//  - UnregisterAll called from a thread other than the render thread returns only once the render thread has left any
//    lock-free dispatch that may still run the removed callbacks (bounded wait, 1 s, logged if it expires). Do not hold a
//    lock that a draw / state callback takes while unregistering from another thread.
#include <d3d9.h>
#include <cstdint>
#include <functional>
#include <string>

namespace D3D9Hooks {

enum class Priority : int { First = 0, Early = 25, Normal = 50, Late = 75, Last = 100 }; // any int is allowed
enum class HookAction { Continue, Skip, Block };

struct DeviceContext {
    IDirect3DDevice9* device = nullptr;
};

using DrawIndexedPrimitiveHook = std::function<HookAction(DeviceContext&, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)>;
using DrawPrimitiveHook = std::function<HookAction(DeviceContext&, D3DPRIMITIVETYPE, UINT, UINT)>;
using SetRenderTargetHook = std::function<HookAction(DeviceContext&, DWORD, IDirect3DSurface9*)>;
using SetPixelShaderHook = std::function<HookAction(DeviceContext&, IDirect3DPixelShader9*)>;
using SetVertexShaderHook = std::function<HookAction(DeviceContext&, IDirect3DVertexShader9*)>;
using SetTextureHook = std::function<HookAction(DeviceContext&, DWORD, IDirect3DBaseTexture9*)>;
using PresentHook = std::function<HookAction(DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*)>;
using BeginSceneHook = std::function<HookAction(DeviceContext&)>;
using CreateTextureHook = std::function<HookAction(DeviceContext&, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*)>;
using CreateRenderTargetHook = std::function<HookAction(DeviceContext&, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*)>;
using SetViewportHook = std::function<HookAction(DeviceContext&, const D3DVIEWPORT9*)>;
using CreatePixelShaderHook = std::function<HookAction(DeviceContext&, const DWORD*, IDirect3DPixelShader9**)>;
using CreateVertexShaderHook = std::function<HookAction(DeviceContext&, const DWORD*, IDirect3DVertexShader9**)>;
using SetPixelShaderConstantFHook = std::function<HookAction(DeviceContext&, UINT, const float*, UINT)>;
using SetVertexShaderConstantFHook = std::function<HookAction(DeviceContext&, UINT, const float*, UINT)>;

bool RegisterDrawIndexedPrimitive(const std::string& name, DrawIndexedPrimitiveHook hook, Priority priority = Priority::Normal);
bool RegisterDrawPrimitive(const std::string& name, DrawPrimitiveHook hook, Priority priority = Priority::Normal);
bool RegisterSetRenderTarget(const std::string& name, SetRenderTargetHook hook, Priority priority = Priority::Normal);
bool RegisterSetPixelShader(const std::string& name, SetPixelShaderHook hook, Priority priority = Priority::Normal);
bool RegisterSetVertexShader(const std::string& name, SetVertexShaderHook hook, Priority priority = Priority::Normal);
bool RegisterSetTexture(const std::string& name, SetTextureHook hook, Priority priority = Priority::Normal);
bool RegisterPresent(const std::string& name, PresentHook hook, Priority priority = Priority::Normal);
bool RegisterBeginScene(const std::string& name, BeginSceneHook hook, Priority priority = Priority::Normal);
bool RegisterCreateTexture(const std::string& name, CreateTextureHook hook, Priority priority = Priority::Normal);
bool RegisterCreateRenderTarget(const std::string& name, CreateRenderTargetHook hook, Priority priority = Priority::Normal);
bool RegisterSetViewport(const std::string& name, SetViewportHook hook, Priority priority = Priority::Normal);
bool RegisterCreatePixelShader(const std::string& name, CreatePixelShaderHook hook, Priority priority = Priority::Normal);
bool RegisterCreateVertexShader(const std::string& name, CreateVertexShaderHook hook, Priority priority = Priority::Normal);
bool RegisterSetPixelShaderConstantF(const std::string& name, SetPixelShaderConstantFHook hook, Priority priority = Priority::Normal);
bool RegisterSetVertexShaderConstantF(const std::string& name, SetVertexShaderConstantFHook hook, Priority priority = Priority::Normal);

// Removes every callback registered under this name (all methods).
void UnregisterAll(const std::string& name);

// The device methods without any Apex callback: the next hook in the Detours chain (a module that detoured the same
// function BEFORE Apex, e.g. official S3SS when it attached first), or the driver. A module that detoured it AFTER Apex
// (outer) does not see these calls either. Before Install they call through the device's vtable.
// Apex's own internal state changes around a replaced draw use these (Night Lighting, lot_light_bridge.cpp), so they
// never re-enter Apex's own chains; the replaced draw itself is still re-issued through the device (its observers:
// Post-scene / Picture trigger counts, Light Probe, Frame Capture, Frame Profiler).
HRESULT CallOriginalCreateRenderTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, DWORD quality, BOOL lockable,
                                       IDirect3DSurface9** surface, HANDLE* shared);
HRESULT CallOriginalSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface);
HRESULT CallOriginalSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport);
HRESULT CallOriginalDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT primCount);
HRESULT CallOriginalDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount);
HRESULT CallOriginalSetVertexShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT count);
HRESULT CallOriginalSetPixelShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT count);
HRESULT CallOriginalSetPixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
HRESULT CallOriginalCreatePixelShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DPixelShader9** shader);
HRESULT CallOriginalCreateVertexShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DVertexShader9** shader);
HRESULT CallOriginalSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader);
HRESULT CallOriginalSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture);

// ---- development build: counters for the Frame Profiler (zeros in the public build) ----
// Calls that reached Apex's detours (the game's calls, and other modules' calls made through the device), counted
// per method in the detours themselves with plain per-thread-unsafe increments (a statistic). Monotonic; the profiler
// takes differences.
struct StateCallCounts {
    uint32_t setTexture = 0, setVertexShader = 0, setPixelShader = 0, setVertexConstants = 0, setPixelConstants = 0, setRenderTarget = 0;
};
StateCallCounts ReadStateCallCounts();
// Dispatches of the lock-free chains that came from a thread other than the render thread (they took the lock).
uint32_t OffThreadDispatches();

// Framework (d3d9_bootstrap.cpp): attach the detours to the game's device (render thread, once) / detach (FreeLibrary).
bool Install(IDirect3DDevice9* device);
bool IsInstalled();
void Uninstall();

} // namespace D3D9Hooks
