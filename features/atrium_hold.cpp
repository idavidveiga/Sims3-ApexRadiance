// An atrium's stories change together (see atrium_hold.h).
#include "atrium_hold.h"
#include "apex_log.h"
#include "apex_util.h"
#include "lamp_mark_filter.h"
#include "level_light_share.h"
#include "room_light_queue.h"
#include "lightmap_smooth.h"
#include "recorder.h"
#include "memory_patch.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

using LockRect_t = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using UnlockRect_t = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT);
LockRect_t g_origLock = nullptr;
UnlockRect_t g_origUnlock = nullptr;

constexpr DWORD kHoldMs = 1500;  // the longest a map waits for the other stories of its atrium
constexpr DWORD kKeepMs = 3000;  // a map's buffers are kept this long after its last change
constexpr size_t kMaxMaps = 192; // maps kept at once (a switch of every lamp of the atrium house: about 150)
constexpr size_t kMaxBytes = 96u << 20; // their buffers together (2 x w x h x 4 each); past it a map is left to the game

// ---- Lamp switches all at once (06/10 evening, user chose "one change only" over the quick pass and its corrections) ----
// From a player's switch until every room it marked on a story its lot shows has ended a solve begun after the switch
// (LevelLightShare::SwitchRoomsPending; the quick pass is off meanwhile, so that is the room's final light), or
// kSwitchHoldMs after the latest switch: every map those rooms' solves write waits as an atrium's maps do, the furniture
// keeps its rig lights (lot_light_bridge.cpp), the ground keeps its smoothed maps (LightmapSmooth) and the per-pixel lamps
// their list (LotLightBridge); then all of them change in the same frame.
std::atomic<bool> g_allAtOnce{true}, g_switchHolding{false};
unsigned long g_switchSeen = 0;              // the latest switch handled (LampMarkFilter::SwitchLastTick)
DWORD g_switchFirst = 0, g_switchLatest = 0; // the hold's first and latest switch
unsigned long g_switchLatestSerial = 0;      // LampMarkFilter::SwitchSerial at the latest switch (for SwitchRoomsPending)
constexpr DWORD kSwitchMinMs = 150, kSwitchHoldMs = 2500, kSwitchMaxMs = 4000;
std::atomic<long> g_switchHolds{0}, g_switchTimeouts{0}, g_switchLastMs{-1}, g_switchLastRooms{0};
std::atomic<DWORD> g_switchUpdated{0}; // the last UpdateSwitchHold: a hold not updated for 1 s (the queue stopped) is over
// On, and the game's solve hooks are in (its end and its map lock step): without them no room would ever count as solved
// and every switch would wait the whole kSwitchHoldMs (other game builds: the switches change room by room)
bool AllAtOnceUsable() { return g_allAtOnce.load(std::memory_order_relaxed) && LevelLightShare::SolveHooksReady(); }
// A switch is held, or one happened that UpdateSwitchHold has not seen yet: a room's solve may lock its maps in the very
// frame of the switch, before the next Present starts the hold (render thread). Never once the queue stopped (Clear) or
// stopped calling UpdateSwitchHold: nothing would release what waits.
bool SwitchPendingNow() {
    if (!AllAtOnceUsable()) return false;
    const DWORD now = GetTickCount();
    if (now - g_switchUpdated.load(std::memory_order_relaxed) >= 1000) return false;
    if (g_switchHolding.load(std::memory_order_relaxed)) return true;
    const unsigned long last = LampMarkFilter::SwitchLastTick();
    return last && last != g_switchSeen && now - last < 1000;
}

struct Map {
    UINT w = 0, h = 0;
    std::vector<uint32_t> shown, latest; // what is on screen while the map waits, the game's exact content
    DWORD last = 0, heldAt = 0;          // last change, start of the wait
    bool held = false;     // waiting for the other stories of its atrium: the screen shows "shown"
    bool release = false;  // its wait is over: "latest" is written at the next frame
    bool resume = false;   // the game's lock got "latest" back: its unlock decides again
    bool refining = false; // written by a refinement (another class's maps, not on screen): never waits
    bool sw = false;       // waits for a lamp switch shown all at once (not for its atrium)
    const void* room = nullptr; // the room whose solve wrote it last (a key: only passed to LevelLightShare)
    int gameLocks = 0;
    RECT rect{};
    DWORD flags = 0;
    BYTE* bits = nullptr; // the game's lock (its rect's top-left) while it holds the map
    INT pitch = 0;
};

std::mutex g_mx;
std::unordered_map<IDirect3DTexture9*, Map> g_maps; // AddRef'd while kept
size_t g_bytes = 0;                                 // their buffers (g_mx)
std::atomic<size_t> g_count{0};
std::atomic<bool> g_active{false}, g_hooked{false}, g_hookTried{false};
thread_local bool t_own = false;
std::atomic<long> g_gameLocks{0}, g_peak{0}, g_held{0}, g_released{0}, g_timeouts{0}, g_refinements{0}, g_writes{0};

// A MANAGED single-level A8R8G8B8 texture of a room map's size (docs/engine/room-light-maps.md, room_map_padding.cpp)
bool RoomMap(IDirect3DTexture9* t, UINT& w, UINT& h) {
    if (!t || t->GetLevelCount() != 1) return false;
    D3DSURFACE_DESC d{};
    if (FAILED(t->GetLevelDesc(0, &d))) return false;
    w = d.Width;
    h = d.Height;
    return d.Format == D3DFMT_A8R8G8B8 && d.Pool == D3DPOOL_MANAGED && w >= 8 && h >= 8 && w <= 1024 && h <= 1024;
}

RECT Clip(const Map& m, const RECT* r) {
    RECT c{0, 0, static_cast<LONG>(m.w), static_cast<LONG>(m.h)};
    if (r) {
        c.left = std::clamp<LONG>(r->left, 0, m.w);
        c.top = std::clamp<LONG>(r->top, 0, m.h);
        c.right = std::clamp<LONG>(r->right, c.left, m.w);
        c.bottom = std::clamp<LONG>(r->bottom, c.top, m.h);
    }
    return c;
}

// rows of a rect between a full-size buffer and locked memory (bits: the rect's top-left)
void ToMemory(const Map& m, const std::vector<uint32_t>& buf, const RECT& r, BYTE* bits, INT pitch) {
    const size_t n = static_cast<size_t>(r.right - r.left) * 4;
    for (LONG y = r.top; y < r.bottom; y++) std::memcpy(bits + static_cast<size_t>(y - r.top) * pitch, &buf[static_cast<size_t>(y) * m.w + r.left], n);
}
bool FromMemory(const Map& m, std::vector<uint32_t>& buf, const RECT& r, const BYTE* bits, INT pitch) {
    const size_t n = static_cast<size_t>(r.right - r.left) * 4;
    bool changed = false;
    for (LONG y = r.top; y < r.bottom; y++) {
        uint32_t* dst = &buf[static_cast<size_t>(y) * m.w + r.left];
        const BYTE* src = bits + static_cast<size_t>(y - r.top) * pitch;
        if (std::memcmp(dst, src, n) != 0) {
            std::memcpy(dst, src, n);
            changed = true;
        }
    }
    return changed;
}
// The whole map takes "latest" (Apex's own lock); false when the lock failed
bool WriteLatest(IDirect3DTexture9* tex, const Map& m) {
    D3DLOCKED_RECT lr{};
    t_own = true;
    const bool ok = SUCCEEDED(g_origLock(tex, 0, &lr, nullptr, 0));
    if (ok) {
        for (UINT y = 0; y < m.h; y++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &m.latest[static_cast<size_t>(y) * m.w], m.w * 4);
        g_origUnlock(tex, 0);
    }
    t_own = false;
    return ok;
}

HRESULT __stdcall LockHook(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    if (t_own || level != 0 || (!g_active.load(std::memory_order_relaxed) && !g_count.load(std::memory_order_relaxed) && !SwitchPendingNow()))
        return g_origLock(t, level, out, rect, flags);
    UINT w = 0, h = 0;
    const bool refining = RoomLightQueue::SolveRefiningUp();
    const void* solving = RoomLightQueue::SolvingRoom();
    std::unique_lock<std::mutex> lk(g_mx);
    auto it = g_maps.find(t);
    if (it == g_maps.end()) {
        // a map a lamp switch's room or an atrium room's solve locks in its step 1 while a lamp edit is pending: its content
        // as it is now (before the solve writes it). Only in that step (LevelLightShare::InMapLockStep), as a room stays
        // mid-solve across frames: never the UI or anything else the render thread locks meanwhile (06/10 review). A
        // switch's room is kept from the very frame of the switch, before the next Present sets g_active (review, H2).
        const bool switchRoom = solving && SwitchPendingNow() && LevelLightShare::LampUrgency(solving) > 1.0f;
        if (refining || !solving || !LevelLightShare::InMapLockStep() || g_maps.size() >= kMaxMaps || !RoomMap(t, w, h) ||
            g_bytes + static_cast<size_t>(w) * h * 8 > kMaxBytes ||
            !(switchRoom || (g_active.load(std::memory_order_relaxed) && LevelLightShare::InAtrium(solving)))) {
            lk.unlock();
            return g_origLock(t, level, out, rect, flags);
        }
        D3DLOCKED_RECT lr{};
        if (FAILED(g_origLock(t, 0, &lr, nullptr, D3DLOCK_READONLY))) {
            lk.unlock();
            return g_origLock(t, level, out, rect, flags);
        }
        Map m;
        m.w = w;
        m.h = h;
        try {
            m.latest.resize(static_cast<size_t>(w) * h);
            FromMemory(m, m.latest, RECT{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)}, static_cast<const BYTE*>(lr.pBits), lr.Pitch);
            m.shown = m.latest;
        } catch (...) { // no memory for its copies: the game keeps the map as it writes it (a throw here would end the game)
            g_origUnlock(t, 0);
            lk.unlock();
            return g_origLock(t, level, out, rect, flags);
        }
        g_origUnlock(t, 0);
        m.last = GetTickCount();
        t->AddRef();
        g_bytes += static_cast<size_t>(w) * h * 8;
        it = g_maps.emplace(t, std::move(m)).first;
        g_count.store(g_maps.size(), std::memory_order_relaxed);
        g_peak.store(std::max<long>(g_peak.load(), static_cast<long>(g_maps.size())), std::memory_order_relaxed);
    }
    const HRESULT hr = g_origLock(t, level, out, rect, flags);
    if (FAILED(hr) || !out || !out->pBits) return hr;
    Map& m = it->second;
    g_gameLocks.fetch_add(1, std::memory_order_relaxed);
    if (m.held || m.release) m.resume = true; // the screen shows "shown": the game gets its exact content
    if (refining) m.refining = true;
    if (solving) m.room = solving;
    m.rect = Clip(m, rect);
    m.flags = flags;
    m.gameLocks++;
    if (m.resume) ToMemory(m, m.latest, m.rect, static_cast<BYTE*>(out->pBits), out->Pitch);
    m.bits = static_cast<BYTE*>(out->pBits);
    m.pitch = out->Pitch;
    return hr;
}

HRESULT __stdcall UnlockHook(IDirect3DTexture9* t, UINT level) {
    if (t_own || level != 0 || !g_count.load(std::memory_order_relaxed)) return g_origUnlock(t, level);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const auto it = g_maps.find(t);
        if (it != g_maps.end() && it->second.gameLocks > 0) {
            Map& m = it->second;
            m.gameLocks--;
            bool changed = false;
            if (m.bits && !(m.flags & D3DLOCK_READONLY)) changed = FromMemory(m, m.latest, m.rect, m.bits, m.pitch);
            if (m.refining) { // another class's maps: shown only when the solve ends, already with this content
                if (changed || m.resume) g_refinements.fetch_add(1, std::memory_order_relaxed);
                m.shown = m.latest;
                m.held = m.release = m.resume = m.refining = false;
            } else if (changed || m.resume) {
                // a lamp switch shown all at once: every map of its rooms waits for the switch; else an atrium's for its stories
                m.sw = m.room && SwitchPendingNow() && LevelLightShare::LampUrgency(m.room) > 1.0f;
                const bool wait = m.room && !LevelLightShare::LampDragging() && (m.sw || LevelLightShare::GroupPending(m.room));
                if (wait) { // what was on screen goes back until the atrium's other stories are solved
                    if (m.bits) ToMemory(m, m.shown, m.rect, m.bits, m.pitch);
                    if (!m.held) {
                        g_held.fetch_add(1, std::memory_order_relaxed);
                        m.heldAt = GetTickCount();
                    }
                    m.held = true;
                    m.release = false;
                } else { // the game's content stays as it wrote it
                    m.shown = m.latest;
                    m.held = m.release = false;
                }
                m.resume = false;
            }
            m.last = GetTickCount();
            m.bits = nullptr;
        }
    }
    return g_origUnlock(t, level);
}

// The file name of the module holding that code ("d3d9.dll" for DXVK and Windows' own)
std::wstring ModuleOf(const void* code) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(code), &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH))
        return L"";
    std::wstring name = path;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    for (wchar_t& c : name) c = towlower(c);
    return name;
}

bool InstallHooks(IDirect3DDevice9* dev) {
    IDirect3DTexture9* probe = nullptr;
    if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) || !probe) return false;
    void** vt = *reinterpret_cast<void***>(probe);
    g_origLock = reinterpret_cast<LockRect_t>(vt[19]);
    g_origUnlock = reinterpret_cast<UnlockRect_t>(vt[20]);
    probe->Release();
    // Only the texture code of a d3d9.dll (DXVK, Windows' own) is patched: a wrapper's (dxwrapper.dll, 06/10: a player's
    // game crashed in it after the first loading screen) may fold identical methods of other interfaces into these two,
    // and the device hooks refuse folded slots for the same reason (D3D9Hooks::Install)
    const std::wstring lockIn = ModuleOf(reinterpret_cast<const void*>(g_origLock)), unlockIn = ModuleOf(reinterpret_cast<const void*>(g_origUnlock));
    if (lockIn != L"d3d9.dll" || unlockIn != L"d3d9.dll" || reinterpret_cast<void*>(g_origLock) == reinterpret_cast<void*>(g_origUnlock)) {
        LOG_INFO(std::format("[AtriumHold] Texture locks are in {}, not a d3d9.dll: left alone (lamp switches change room by room)",
                             ApexUtil::ToUtf8(lockIn)));
        g_origLock = nullptr;
        g_origUnlock = nullptr;
        return false;
    }
    std::vector<DetourBatch::Hook> hooks = {{reinterpret_cast<void**>(&g_origLock), reinterpret_cast<void*>(&LockHook)},
                                            {reinterpret_cast<void**>(&g_origUnlock), reinterpret_cast<void*>(&UnlockHook)}};
    return DetourBatch::InstallHooks(hooks);
}

// Render thread, every frame: a new player switch starts (or extends) the hold; it ends once every room the switch marked on a
// shown story has its final light, kSwitchHoldMs after the latest switch, or kSwitchMaxMs after the first
void UpdateSwitchHold() {
    if (!AllAtOnceUsable()) {
        g_switchHolding.store(false, std::memory_order_relaxed);
        return;
    }
    const DWORD now = GetTickCount();
    g_switchUpdated.store(now, std::memory_order_relaxed);
    const unsigned long last = LampMarkFilter::SwitchLastTick();
    if (last && last != g_switchSeen) { // a switch since the last frame (an old one, e.g. before the option was on, starts nothing)
        g_switchSeen = last;
        if (now - last < 1000) {
            if (!g_switchHolding.load(std::memory_order_relaxed)) {
                g_switchFirst = last;
                g_switchHolds.fetch_add(1, std::memory_order_relaxed);
            }
            g_switchLatest = last;
            g_switchLatestSerial = LampMarkFilter::SwitchSerial();
            g_switchHolding.store(true, std::memory_order_relaxed);
        }
    }
    if (!g_switchHolding.load(std::memory_order_relaxed)) return;
    const DWORD sinceLatest = now - g_switchLatest, sinceFirst = now - g_switchFirst;
    if (sinceLatest < kSwitchMinMs) return; // the game's marks and the safety net send the rooms first
    int visible = 0;
    const int pending = LevelLightShare::SwitchRoomsPending(g_switchLatestSerial, &visible) + (LightmapSmooth::HoldPending() ? 1 : 0); // and the ground's chunks
    const bool timeout = sinceLatest >= kSwitchHoldMs || sinceFirst >= kSwitchMaxMs;
    if (pending > 0 && !timeout) return;
    if (pending > 0) g_switchTimeouts.fetch_add(1, std::memory_order_relaxed);
    g_switchLastMs.store(static_cast<long>(sinceFirst), std::memory_order_relaxed);
    g_switchLastRooms.store(visible, std::memory_order_relaxed);
    if (Recorder::Active()) // the recording's timeline: when the switch was shown
        Recorder::Note(std::format("[switch] lamp switch shown all at once, {} ms after its first switch ({} rooms on screen{})", sinceFirst, visible,
                                   pending > 0 ? std::format(", {} still waiting: shown after {} ms", pending, kSwitchHoldMs) : std::string()));
    g_switchHolding.store(false, std::memory_order_relaxed);
}

} // namespace

namespace AtriumHold {

void SetAllAtOnce(bool on) {
    if (!g_allAtOnce.exchange(on) || on) return;
    g_switchHolding.store(false, std::memory_order_relaxed); // switched off during a hold: it ends now
}
bool AllAtOnce() { return AllAtOnceUsable() && g_hooked.load(std::memory_order_relaxed); }
// held, or a switch in this very frame, before the next Present starts the hold: the furniture and lamps already wait
bool SwitchHolding() { return g_hooked.load(std::memory_order_relaxed) && SwitchPendingNow(); }

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_hookTried.exchange(true)) {
        const bool ok = dev && InstallHooks(dev);
        g_hooked.store(ok);
        LOG_INFO(ok ? "[AtriumHold] Texture lock hooks installed: an atrium's stories take their new light together during lamp edits"
                    : "[AtriumHold] Texture lock hooks not available: an atrium's stories change as each is solved");
    }
    if (!g_hooked.load()) {
        g_switchHolding.store(false, std::memory_order_relaxed);
        return;
    }
    UpdateSwitchHold();
    g_active.store(LevelLightShare::LampEditPending() || LampMarkFilter::MassSwitchActive() || g_switchHolding.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
    if (!g_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mx);
    const DWORD now = GetTickCount();
    // every waiting map whose atrium is solved, or whose lamp switch is over, takes its content in this frame
    const bool switching = g_switchHolding.load(std::memory_order_relaxed);
    for (auto& [tex, m] : g_maps) {
        if (!m.held || m.gameLocks != 0) continue;
        if (now - m.heldAt >= (m.sw ? kSwitchMaxMs : kHoldMs)) g_timeouts.fetch_add(1, std::memory_order_relaxed);
        else if (m.sw ? switching : (m.room && LevelLightShare::GroupPending(m.room))) continue;
        m.held = false;
        m.release = true;
        g_released.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto it = g_maps.begin(); it != g_maps.end();) {
        Map& m = it->second;
        IDirect3DTexture9* tex = it->first;
        if (m.gameLocks > 0 || m.held) { ++it; continue; } // never written while the game holds it; held: shows "shown"
        if (m.release) {
            if (WriteLatest(tex, m)) g_writes.fetch_add(1, std::memory_order_relaxed);
            m.shown = m.latest;
            m.release = false;
            ++it;
            continue;
        }
        if (now - m.last > kKeepMs) { // idle: buffers and the reference go
            tex->Release();
            g_bytes -= std::min(g_bytes, static_cast<size_t>(m.w) * m.h * 8);
            it = g_maps.erase(it);
            continue;
        }
        ++it;
    }
    g_count.store(g_maps.size(), std::memory_order_relaxed);
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& [tex, m] : g_maps) {
        if ((m.held || m.release) && m.gameLocks == 0) WriteLatest(tex, m); // a wait cut short: the map gets its content
        tex->Release();
    }
    g_maps.clear();
    g_bytes = 0;
    g_count.store(0, std::memory_order_relaxed);
    // the queue stopped (or the world changed): nothing waits or is kept until the next Present runs the hold again (review
    // 06/10: a switch after Faster Room Lighting went off kept capturing maps nobody released)
    g_active.store(false, std::memory_order_relaxed);
    g_switchHolding.store(false, std::memory_order_relaxed);
    g_switchUpdated.store(GetTickCount() - 1000, std::memory_order_relaxed);
}

std::string Status() {
    if (!g_hooked.load()) return g_hookTried.load() ? "not available" : "waiting for the device";
    const long lastMs = g_switchLastMs.load();
    return std::format("maps kept {} (at most {} at once), maps that waited for their other stories or their switch {} (released together {}, "
                       "after the longest wait {}), refinements left alone {}, writes {}, game locks seen {} | lamp switches all at once: {}, {} shown{}{}{}",
                       g_count.load(), g_peak.load(), g_held.load(), g_released.load(), g_timeouts.load(), g_refinements.load(),
                       g_writes.load(), g_gameLocks.load(), g_allAtOnce.load() ? "on" : "off", g_switchHolds.load(),
                       lastMs >= 0 ? std::format(" (the last after {} ms, {} rooms on screen)", lastMs, g_switchLastRooms.load()) : std::string(),
                       g_switchTimeouts.load() ? std::format(", {} after {} ms", g_switchTimeouts.load(), kSwitchHoldMs) : std::string(),
                       g_switchHolding.load() ? " | holding a switch" : "");
}

} // namespace AtriumHold
