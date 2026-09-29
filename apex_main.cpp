// Apex Radiance for The Sims 3: ApexRadiance.asi, a standalone ASI for The Sims 3 (TS3W.exe) that runs next to an
// unmodified official Sims3SettingsSetter.
//
// Start-up:
//  DllMain      process check, instance mutexes (a second copy, or an older S3SSApex.asi that loaded first, keeps this
//               copy idle), Direct3DCreate9 export detour (d3d9_bootstrap.h), init thread.
//  init thread  ApexRadiance_LOG.txt, game version, S3SS detection, features created, one-time migration (previous
//               S3SS\Apex\Apex.toml, else S3SS.toml), settings (menu, display, Picture, profiler); then waits until
//               the game settled (first Present + 1 s, so official S3SS has loaded its own patches), checks for the
//               old combined build and an older S3SSApex.asi, resolves the game-code addresses (game_addresses.h:
//               fixed on Steam 1.67.2, signature scan elsewhere), installs the enabled features, and becomes the pump:
//               every 10 ms each feature's Update() and the config autosave.
//  Shutdown     only on FreeLibrary (never at process exit, where the loader may hold other threads' locks).
#include <windows.h>
#include "apex_config.h"
#include "apex_gui.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "conflict_guard.h"
#include "d3d9_bootstrap.h"
#include "frame_profiler.h"
#include "game_addresses.h"
#include "game_version.h"
#include "overlay.h"
#include "patch_base.h"
#include "s3ss_detect.h"
#include <format>
#include <string>

namespace {

HMODULE g_module = nullptr;
HANDLE g_stop = nullptr; // set on FreeLibrary: the init / pump thread ends
HANDLE g_thread = nullptr;

constexpr DWORD kPumpIntervalMs = 10;
constexpr ULONGLONG kSettleAfterPresentMs = 1000; // official S3SS loads its patches from its hook thread at startup
constexpr ULONGLONG kSettleTimeoutMs = 20000;     // no Present by then (no device, headless): install anyway

bool Stopping(DWORD waitMs = 0) { return g_stop && WaitForSingleObject(g_stop, waitMs) == WAIT_OBJECT_0; }

std::wstring ModuleName(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

void OpenLog() {
    if (ApexPaths::EnsureApexDirectory() && ApexLog::Open(ApexPaths::LogFile())) return;
    // Documents not reachable: next to the game (never S3SS_LOG.txt)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    ApexLog::Open(dir + L"ApexRadiance_LOG.txt");
}

// An older S3SSApex.asi loaded first and runs: this copy stays idle (no hooks, no menu) and only says why in its log.
DWORD WINAPI IdleNoticeThread(LPVOID) {
    OpenLog();
    LOG_ERROR("[Main] An older S3SSApex.asi is also installed; delete it from Game\\Bin. It loaded first, so this copy of " APEX_PRODUCT_NAME
              " " APEX_VERSION_STRING " stays idle (no features, no menu) until it is removed.");
    ApexLog::Close();
    return 0;
}

// Waits until the game has presented a frame and a moment has passed (or the timeout). False when stopping.
bool WaitForSettle() {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (Stopping(50)) return false;
        const ULONGLONG now = GetTickCount64();
        if (ApexD3D::PresentSeen() && now - ApexD3D::FirstPresentTick() >= kSettleAfterPresentMs) return true;
        if (now - start >= kSettleTimeoutMs) {
            LOG_WARNING("[Main] No Present after 20 s: installing the features anyway");
            return true;
        }
    }
}

DWORD WINAPI InitThread(LPVOID) {
    OpenLog();
    LOG_INFO(std::format("[Main] {} {} ({}) in {}", APEX_PRODUCT_NAME, APEX_VERSION_STRING, kPublicBuild ? "public build" : "development build",
                         ApexUtil::ToUtf8(ModuleName(nullptr))));
    LOG_INFO("[Main] Files: " + ApexUtil::ToUtf8(ApexPaths::ApexDirectory()));
    if (DetectGameVersion()) LOG_INFO(std::format("[Main] Game: {} [0x{:08X}]", GetGameVersionName(), g_exeTimestamp));
    else LOG_WARNING(std::format("[Main] Unknown game build [0x{:08X}]: game-code features start only where their code is found by signature", g_exeTimestamp));

    S3SSDetect::Scan();
    ApexD3D::EnsureInstalled(); // only does something when the DllMain install could not happen

    try {
        PatchManager::Get().CreateAll();
        ApexConfig::EnsureMigrated();
        ApexConfig::LoadSettings();
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[Main] Settings could not be loaded: ") + e.what());
    }

    if (!WaitForSettle()) return 0;
    const S3SSDetect::Info& s3ss = S3SSDetect::Rescan();
    if (s3ss.oldStandalone) {
        // It loaded after this build, found Local\S3SSApex.<pid> taken and idles; it still has to go.
        LOG_WARNING("[Main] An older " + ApexUtil::ToUtf8(s3ss.oldStandaloneModule) + " is also installed; delete it from Game\\Bin (it stays idle meanwhile)");
        ApexGui::SetOldStandaloneNotice(ApexUtil::ToUtf8(s3ss.oldStandaloneModule));
    }
    if (s3ss.oldCombinedBuild) {
        LOG_ERROR("[Main] The old combined build (" + ApexUtil::ToUtf8(s3ss.combinedModule) + ") is loaded too: " APEX_PRODUCT_NAME "'s features stay off. Delete it from Game\\Bin.");
        ApexGui::SetStartup(ApexGui::Startup::RefusedOldBuild, ApexUtil::ToUtf8(s3ss.combinedModule));
    } else {
        // Game-code addresses: fixed on Steam 1.67.2, found by signature on other builds (game code decrypted by now)
        GameAddr::Resolve();
        try {
            ApexConfig::LoadFeatures();
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("[Main] Features could not be started: ") + e.what());
        }
        ApexGui::SetStartup(ApexGui::Startup::Running);
        LOG_INFO("[Main] Features started");
    }

    // Pump: features' periodic work (deferred reinstalls, lamp scans scheduled off the render thread) and autosave.
    ULONGLONG lastGuardTick = 0;
    while (!Stopping(kPumpIntervalMs)) {
        if (ApexGui::GetStartup() == ApexGui::Startup::Running) PatchManager::Get().UpdateAll();
        ApexConfig::PumpAutosave();
        const ULONGLONG now = GetTickCount64();
        if (now - lastGuardTick >= 1000) {
            lastGuardTick = now;
            ConflictGuard::Tick(); // TODO(step 8): watchdog
        }
    }
    return 0;
}

bool IsGameProcess() {
    const std::wstring exe = ModuleName(nullptr);
    return _wcsicmp(exe.c_str(), L"TS3W.exe") == 0 || _wcsicmp(exe.c_str(), L"TS3.exe") == 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        g_module = module;
        if (!IsGameProcess()) return FALSE; // the launcher and other tools: not loaded at all
        switch (S3SSDetect::AcquireInstanceMutex()) {
        case S3SSDetect::Instance::DuplicateSelf:
            OutputDebugStringA("[" APEX_PRODUCT_NAME "] Another copy of ApexRadiance.asi is already running in this process: this one stays idle\n");
            return TRUE;
        case S3SSDetect::Instance::OldStandaloneFirst:
            OutputDebugStringA("[" APEX_PRODUCT_NAME "] An older S3SSApex.asi is also installed and loaded first: this one stays idle. Delete it from Game\\Bin\n");
            if (HANDLE t = CreateThread(nullptr, 0, IdleNoticeThread, nullptr, 0, nullptr)) CloseHandle(t);
            return TRUE;
        default:
            break;
        }
        LOG_INFO(std::format("[Main] Loaded as {}", ApexUtil::ToUtf8(ModuleName(module))));
        Overlay::SetClient(&ApexGui::Client());
        ApexD3D::InstallFromDllMain(); // d3d9.dll is a static import of TS3W.exe: normally already loaded here
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (!g_thread) OutputDebugStringA("[" APEX_PRODUCT_NAME "] Could not start the init thread\n");
        break;
    }
    case DLL_PROCESS_DETACH:
        if (!reserved && g_thread) { // FreeLibrary (not process exit)
            SetEvent(g_stop);        // never waited for inside DllMain
            FrameProfiler::Shutdown();
            PatchManager::Get().UninstallAll();
            ApexD3D::Shutdown();
            ApexLog::Close();
        }
        break;
    default:
        break;
    }
    return TRUE;
}
