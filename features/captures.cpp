// Bug-report captures: folders, the files copied with them and the on-screen notes (see captures.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "captures.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "game_version.h"
#include "ui/i18n.h"
#include "render_callbacks.h"
#include "d3d9_hooks.h"
#include "overlay.h"
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace Captures {
namespace {

std::mutex g_lock;
std::string g_note;
unsigned long long g_noteUntil = 0;
std::filesystem::path g_session; // the open session's folder (empty: none); under g_lock
int g_sessionCount = 0;
unsigned long long g_sessionStart = 0; // GetTickCount64 when it was opened
std::vector<std::string> g_sessionItems; // what each capture of the session was

std::filesystem::path Dir() { return std::filesystem::path(ApexPaths::ApexDirectory()); }

// Explorer on a folder, on a short-lived thread with COM (30/09: called from the menu frame, ShellExecuteW pumped the
// game window's messages, the overlay's window procedure ran again inside the frame and its mutex threw: a crash)
void ShowInExplorer(const std::filesystem::path& folder) {
    std::thread([folder] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) LOG_WARNING(std::format("[Captures] Could not open the folder ({})", reinterpret_cast<INT_PTR>(r)));
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

void CopyIfThere(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (std::filesystem::exists(from, ec)) std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
}

// ---- Screenshots (user, 30/09: "the game should capture the screen too, to have a print") ----
// Finish queues the capture folder; the next frame's picture is copied from the back buffer and written as
// Screenshot.png on a short-lived thread (WIC). Menu closed: at Present, so the picture has everything the player sees
// (Color filters included); the capture notes are not drawn that frame (ScreenshotPending). Menu open: at the end of the
// scene, before the Apex menu draws (the Color filters come after the menu, so they are not in that one).
std::vector<std::filesystem::path> g_shots; // folders waiting for their screenshot (render thread)
bool g_shotHooks = false;
std::atomic<bool> g_shotsOn{true};

void WritePng(std::filesystem::path file, std::vector<BYTE> bgr, UINT w, UINT h) {
    std::thread([file = std::move(file), bgr = std::move(bgr), w, h] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                  SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
                  SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
                  SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), const_cast<BYTE*>(bgr.data()))) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit());
        if (frame) frame->Release();
        if (enc) enc->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        if (!ok) LOG_WARNING("[Captures] The screenshot could not be written: " + file.string());
    }).detach();
}

// Render thread: the back buffer now -> Screenshot.png in every queued folder
void TakeShots(IDirect3DDevice9* dev) {
    if (g_shots.empty() || !dev) return;
    const std::vector<std::filesystem::path> folders = std::move(g_shots);
    g_shots.clear();
    IDirect3DSurface9 *bb = nullptr, *resolved = nullptr, *sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    std::vector<BYTE> bgr;
    if (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) {
        IDirect3DSurface9* src = bb;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE && SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE)))
            src = resolved;
        if ((d.MultiSampleType == D3DMULTISAMPLE_NONE || src == resolved) &&
            SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && SUCCEEDED(dev->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                bgr.resize(static_cast<size_t>(d.Width) * d.Height * 3);
                for (UINT y = 0; y < d.Height; y++) {
                    const BYTE* s = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                    BYTE* o = bgr.data() + static_cast<size_t>(y) * d.Width * 3;
                    for (UINT x = 0; x < d.Width; x++) { // B G R (A dropped: it is the bloom mask, not transparency)
                        o[3 * x] = s[4 * x];
                        o[3 * x + 1] = s[4 * x + 1];
                        o[3 * x + 2] = s[4 * x + 2];
                    }
                }
                sys->UnlockRect();
            }
        }
    }
    if (sys) sys->Release();
    if (resolved) resolved->Release();
    bb->Release();
    if (bgr.empty()) {
        LOG_WARNING("[Captures] No screenshot: the screen format could not be read");
        return;
    }
    for (size_t i = 0; i < folders.size(); i++) WritePng(folders[i] / L"Screenshot.png", i + 1 < folders.size() ? bgr : std::move(bgr), d.Width, d.Height);
}

void ShotAtSceneEnd(IDirect3DDevice9* dev) {
    if (Overlay::IsVisible()) TakeShots(dev); // the menu is about to be drawn: the picture before it
}

void QueueShot(const std::filesystem::path& folder) {
    if (!g_shotsOn.load()) return;
    g_shots.push_back(folder);
    if (g_shotHooks) return;
    g_shotHooks = true; // once; the callbacks do nothing while nothing is queued
    RenderCallbacks::endSceneBeforeOverlay.Add(ShotAtSceneEnd);
    D3D9Hooks::RegisterPresent("CapturesScreenshot", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        TakeShots(ctx.device); // menu closed: the frame as it is shown (notes held back for it)
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::First);
}

} // namespace

std::filesystem::path Root() { return Dir() / L"Captures"; }

// A new folder "<date time> <kind>" in parent (" (2)", " (3)"... when the name is taken: never reused)
std::filesystem::path MakeFolder(const std::filesystem::path& parent, const char* kind, bool withDate) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(parent, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string base = withDate ? std::format("{:04}-{:02}-{:02} {:02}-{:02}-{:02} {}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind)
                                      : std::format("{:02}-{:02}-{:02} {}", t.wHour, t.wMinute, t.wSecond, kind);
    fs::path p = parent / fs::path(base);
    for (int n = 2; fs::exists(p, ec) && n < 1000; n++) p = parent / fs::path(std::format("{} ({})", base, n));
    fs::create_directories(p, ec);
    return p;
}

std::filesystem::path NewFolder(const char* kind) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_session.empty()) return MakeFolder(g_session, kind, false); // inside the open session
    return MakeFolder(Root(), kind, true);
}

void BeginSession() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_session.empty()) return;
    g_session = MakeFolder(Root(), "Session", true);
    g_sessionCount = 0;
    g_sessionStart = GetTickCount64();
    g_sessionItems.clear();
    LOG_INFO("[Captures] Session started: Captures\\" + g_session.filename().string());
}

void EndSession() {
    std::filesystem::path s;
    std::vector<std::string> items;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        s = g_session;
        items = g_sessionItems;
        g_session.clear();
    }
    if (s.empty()) return;
    CopyIfThere(Dir() / L"ApexRadiance_LOG.txt", s / L"ApexRadiance_LOG.txt");
    CopyIfThere(Dir() / L"ApexRadiance.toml", s / L"ApexRadiance.toml");
    CopyIfThere(Dir() / L"ApexRadiance_Crash.txt", s / L"ApexRadiance_Crash.txt");
    std::ofstream about(s / L"About this session.txt", std::ios::out | std::ios::trunc);
    if (about) {
        about << std::format("{} {} - a capture session with {} captures, on {}.\n\n", APEX_PRODUCT_NAME, APEX_VERSION_STRING, items.size(), GetGameVersionName());
        for (const std::string& i : items) about << "- " << i << "\n";
        about << "\nEach capture is in its own folder here, with the log as it was at that moment; the log and settings at the end of the session are "
                 "beside them.\n\nTo report a problem: right-click this folder > Send to > Compressed (zipped) folder, then attach the .zip to your post "
                 "in the Bugs tab of Apex Radiance on Nexus Mods, or to an issue on GitHub (github.com/loinyx/Sims3-ApexRadiance/issues). Say in a few "
                 "words what you saw and what you did just before.\n";
    }
    const std::string name = s.filename().string();
    LOG_INFO(std::format("[Captures] Session ended: Captures\\{} ({} captures)", name, items.size()));
    Notify(I18n::Trf("Session saved in Captures \xE2\x80\xBA {}", name), 6);
}

bool SessionActive() {
    std::lock_guard<std::mutex> lk(g_lock);
    return !g_session.empty();
}
int SessionCaptures() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_sessionCount;
}
std::string SessionFolder() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_session.empty() ? std::string() : g_session.filename().string();
}
std::vector<std::string> SessionItems() {
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<std::string> names;
    for (const std::string& i : g_sessionItems) names.push_back(i.substr(0, i.find(": "))); // the folder name only
    return names;
}
int SessionSeconds() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_session.empty() ? 0 : static_cast<int>((GetTickCount64() - g_sessionStart) / 1000);
}

void Finish(const std::filesystem::path& folder, const std::string& what) {
    CopyIfThere(Dir() / L"ApexRadiance_LOG.txt", folder / L"ApexRadiance_LOG.txt");
    CopyIfThere(Dir() / L"ApexRadiance.toml", folder / L"ApexRadiance.toml");
    CopyIfThere(Dir() / L"ApexRadiance_Crash.txt", folder / L"ApexRadiance_Crash.txt");
    QueueShot(folder); // Screenshot.png, from the next frame (when screenshots are on)
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::ofstream about(folder / L"About this capture.txt", std::ios::out | std::ios::trunc);
    if (about) {
        about << std::format("{} {} - {}\n", APEX_PRODUCT_NAME, APEX_VERSION_STRING, what);
        about << std::format("Taken {:04}-{:02}-{:02} {:02}:{:02}:{:02} on {}.\n\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetGameVersionName());
        about << "This folder holds everything needed to look into a problem: the capture, the mod's log (ApexRadiance_LOG.txt), your settings "
                 "(ApexRadiance.toml) and, after a crash, ApexRadiance_Crash.txt.\n\n";
        about << "To report a problem: right-click this folder > Send to > Compressed (zipped) folder, then attach the .zip to your post in the "
                 "Bugs tab of Apex Radiance on Nexus Mods, or to an issue on GitHub (github.com/loinyx/Sims3-ApexRadiance/issues). "
                 "Say in a few words what you saw and what you did just before.\n";
    }
    const std::string name = folder.filename().string();
    bool inSession = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_session.empty() && folder.parent_path() == g_session) {
            inSession = true;
            g_sessionCount++;
            g_sessionItems.push_back(name + ": " + what);
        }
    }
    LOG_INFO(std::format("[Captures] Saved: Captures\\{}{} ({})", inSession ? SessionFolder() + "\\" : "", name, what));
    Notify(inSession ? I18n::Trf("Added to the session: {}", name) : I18n::Trf("Saved in Captures \xE2\x80\xBA {}", name), 6);
}

void SetScreenshots(bool on) { g_shotsOn = on; }
bool Screenshots() { return g_shotsOn.load(); }
bool ScreenshotPending() { return !g_shots.empty(); }

void SaveReport() { Finish(NewFolder("Report"), "a report: the log and the settings"); }

std::string RecentCrash() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const std::filesystem::path p = Dir() / L"ApexRadiance_Crash.txt";
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return "";
    ULARGE_INTEGER w{}, now{};
    w.LowPart = a.ftLastWriteTime.dwLowDateTime;
    w.HighPart = a.ftLastWriteTime.dwHighDateTime;
    FILETIME nf;
    GetSystemTimeAsFileTime(&nf);
    now.LowPart = nf.dwLowDateTime;
    now.HighPart = nf.dwHighDateTime;
    if (now.QuadPart < w.QuadPart || now.QuadPart - w.QuadPart > 7ull * 24 * 3600 * 10000000ull) return "";
    FILETIME local;
    SYSTEMTIME t;
    if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &t)) return "";
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
}

void Notify(const std::string& text, int seconds) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_note = text;
    g_noteUntil = GetTickCount64() + static_cast<unsigned long long>(seconds) * 1000ull;
}

Note CurrentNote() {
    std::lock_guard<std::mutex> lk(g_lock);
    Note n;
    if (!g_note.empty() && GetTickCount64() < g_noteUntil) {
        n.text = g_note;
        n.visible = true;
    }
    return n;
}

Summary Scan() {
    namespace fs = std::filesystem;
    Summary s;
    std::error_code ec;
    fs::file_time_type newest{};
    for (const auto& e : fs::directory_iterator(Root(), ec)) {
        if (!e.is_directory(ec)) continue;
        s.count++;
        const auto wt = e.last_write_time(ec);
        if (s.newest.empty() || wt > newest) {
            newest = wt;
            s.newest = e.path().filename().string();
        }
        for (const auto& f : fs::recursive_directory_iterator(e.path(), ec))
            if (f.is_regular_file(ec)) s.bytes += f.file_size(ec);
    }
    return s;
}

void OpenFolder() {
    std::error_code ec;
    std::filesystem::create_directories(Root(), ec);
    ShowInExplorer(Root());
}

int DeleteAll() {
    namespace fs = std::filesystem;
    std::error_code ec;
    int n = 0;
    const std::string open = SessionFolder();
    for (const auto& e : fs::directory_iterator(Root(), ec))
        if (e.is_directory(ec) && e.path().filename().string() != open && fs::remove_all(e.path(), ec) != static_cast<std::uintmax_t>(-1)) n++;
    LOG_INFO(std::format("[Captures] {} capture folders deleted from the menu", n));
    return n;
}

std::vector<Entry> List() {
    namespace fs = std::filesystem;
    std::vector<Entry> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(Root(), ec)) {
        if (!e.is_directory(ec)) continue;
        Entry x;
        x.folder = e.path().filename().string();
        // "YYYY-MM-DD HH-MM-SS kind": names made by NewFolder; other folders are listed by name only
        if (x.folder.size() > 20 && x.folder[4] == '-' && x.folder[10] == ' ' && x.folder[13] == '-') {
            x.date = x.folder.substr(0, 10);
            x.time = x.folder.substr(11, 2) + ":" + x.folder.substr(14, 2) + ":" + x.folder.substr(17, 2);
            x.kind = x.folder.substr(20);
        } else {
            x.kind = x.folder;
        }
        for (const auto& f : fs::recursive_directory_iterator(e.path(), ec))
            if (f.is_regular_file(ec)) x.bytes += f.file_size(ec);
        if (x.kind.rfind("Session", 0) == 0)
            for (const auto& f : fs::directory_iterator(e.path(), ec))
                if (f.is_directory(ec)) x.items++;
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.folder > b.folder; }); // the date first in the name: newest first
    return out;
}

void Open(const std::string& folder) {
    const std::filesystem::path p = Root() / std::filesystem::path(folder);
    ShowInExplorer(p);
}

bool Delete(const std::string& folder) {
    namespace fs = std::filesystem;
    if (folder.empty() || folder.find_first_of("\\/:") != std::string::npos || folder == "." || folder == "..") return false; // only a direct child of Captures\.
    if (folder == SessionFolder()) return false; // the open session: end it first
    std::error_code ec;
    const fs::path p = Root() / fs::path(folder);
    if (!fs::is_directory(p, ec)) return false;
    const bool ok = fs::remove_all(p, ec) != static_cast<std::uintmax_t>(-1) && !ec;
    LOG_INFO(std::format("[Captures] Deleted from the menu: Captures\\{}{}", folder, ok ? "" : " (failed: " + ec.message() + ")"));
    return ok;
}

} // namespace Captures
