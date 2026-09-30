// One graphics driver in the game: keeps an AMD integrated GPU's Vulkan driver out of TS3W (see vulkan_driver_guard.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "vulkan_driver_guard.h"
#include "apex_log.h"
#include <windows.h>
#include <dxgi.h>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace VulkanDriverGuard {
namespace {

constexpr UINT kVendorAmd = 0x1002;
// AMD's 32-bit driver manifests (amd-vulkan32.json; older drivers amdvlk32.json): loader globs, matched on the file name
constexpr const wchar_t* kDisable = L"amd-vulkan32.json,amdvlk32.json";

std::once_flag g_once;
std::string g_decision;

bool EnvSet(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) > 0; }

struct Adapter {
    std::wstring name;
    UINT vendor = 0;
    unsigned long long dedicated = 0;
};

bool ListAdapters(std::vector<Adapter>& out) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return false;
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return false;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        if (a && SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) out.push_back({d.Description, d.VendorId, d.DedicatedVideoMemory});
        if (a) a->Release();
        a = nullptr;
    }
    factory->Release();
    return true;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

std::string Decide() {
    if (GetModuleHandleW(L"vulkan-1.dll")) return "left as it is: the Vulkan loader was already loaded";
    for (const wchar_t* v : {L"VK_LOADER_DRIVERS_DISABLE", L"VK_LOADER_DRIVERS_SELECT", L"VK_DRIVER_FILES", L"VK_ICD_FILENAMES", L"VK_ADD_DRIVER_FILES"})
        if (EnvSet(v)) return "left as it is: a Vulkan driver variable is already set";
    std::vector<Adapter> adapters;
    if (!ListAdapters(adapters)) return "left as it is: the graphics adapters could not be listed";
    const Adapter* main = nullptr;
    bool haveAmd = false;
    for (const Adapter& a : adapters) {
        if (!main || a.dedicated > main->dedicated) main = &a;
        haveAmd |= a.vendor == kVendorAmd;
    }
    std::string list;
    for (const Adapter& a : adapters) list += std::format("{}{} ({:04X}, {} MB)", list.empty() ? "" : ", ", Narrow(a.name), a.vendor, a.dedicated >> 20);
    if (!haveAmd) return "no AMD graphics driver to leave out (" + list + ")";
    if (!main || main->vendor == kVendorAmd) return "left as it is: the main graphics card is AMD (" + list + ")";
    for (const Adapter& a : adapters)
        if (a.vendor == kVendorAmd && a.dedicated * 2 >= main->dedicated) return "left as it is: the AMD adapter is not a small integrated GPU (" + list + ")";
    if (!SetEnvironmentVariableW(L"VK_LOADER_DRIVERS_DISABLE", kDisable)) return "the variable could not be set";
    return "AMD's 32-bit Vulkan driver left out of the game (VK_LOADER_DRIVERS_DISABLE, this process only); the game renders on " + Narrow(main->name) + " (" + list + ")";
}

} // namespace

void BeforeDirect3DCreate() {
    std::call_once(g_once, [] {
        g_decision = Decide();
        LOG_INFO("[VulkanDriverGuard] " + g_decision);
    });
}

const char* Decision() { return g_decision.c_str(); }

} // namespace VulkanDriverGuard
