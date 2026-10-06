// Smooth light changes indoors (see room_light_fade.h).
#include "room_light_fade.h"
#include "apex_log.h"
#include "lamp_mark_filter.h"
#include "level_light_share.h"
#include "room_light_queue.h"
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

constexpr DWORD kFadeMs = 250;   // from what was on screen to the new light
constexpr DWORD kKeepMs = 3000;  // a map's buffers are kept this long after its last change
constexpr size_t kMaxMaps = 128; // maps faded at once (a five-story lot has about forty)

struct Map {
    UINT w = 0, h = 0;
    std::vector<uint32_t> from, to; // what was on screen when the fade (re)started, the game's exact content
    DWORD start = 0, last = 0;      // fade start, last change
    bool fading = false, resume = false;
    bool held = false; // changed while a burst's quick pass runs: still showing "from", its fade starts with every other map's
    bool exact = false; // written by a refinement (see LockHook): its content shows at once, never faded from "from"
    bool groupHeld = false; // held for the other stories of its atrium (see kGroupHoldMs)
    DWORD heldAt = 0;       // when the hold started
    const void* room = nullptr; // the room whose solve wrote it last (a key: only passed to LevelLightShare::GroupPending)
    int gameLocks = 0;
    RECT rect{};
    DWORD flags = 0;
    BYTE* bits = nullptr; // the game's lock (its rect's top-left) while it holds the map
    INT pitch = 0;
};

std::mutex g_mx;
std::unordered_map<IDirect3DTexture9*, Map> g_maps; // AddRef'd while kept
std::atomic<size_t> g_count{0};
std::atomic<bool> g_enabled{true}, g_active{false}, g_hooked{false}, g_hookTried{false};
thread_local bool t_own = false;
std::atomic<long> g_fades{0}, g_writes{0}, g_gameLocks{0}, g_resumes{0}, g_peak{0}, g_heldMaps{0}, g_releases{0}, g_exact{0}, g_groupHolds{0}, g_groupTimeouts{0};
// Every story together (06/10, user: "it applies first on the story of the light, and only later on the others"): while
// the lamp edit's rooms are still taking their first solve (all of them for one lamp; the quick pass for many), a changed
// map holds what was on screen; when they are done, or after kHoldMaxMs, every held map starts its fade in the same frame.
// Not while a lamp is dragged (its light follows it).
std::atomic<bool> g_holding{false};
DWORD g_holdFrom = 0;
constexpr DWORD kHoldMaxMs = 2500;
constexpr bool kHoldStories = false;
// The stories of one atrium together (06/10, video 12:00, user: "some corners look right, then wrong again"): a lamp of
// the atrium switched, the lamp's story changed first, and its wall showed a step at the floor line against the story
// above for about a second, until that story's solve ended. Only the maps of an atrium's rooms (LevelLightShare's
// stacked-ambient groups) wait now, while another room of the same atrium is still waiting for or in its solve, and at
// most kGroupHoldMs; every other room fades at once. Not while a lamp is dragged.
constexpr DWORD kGroupHoldMs = 1500;

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

// The blend of from and to at weight w (0..256), per channel
inline uint32_t Mix(uint32_t a, uint32_t b, uint32_t w) {
    const uint32_t rbA = a & 0x00FF00FFu, gaA = (a >> 8) & 0x00FF00FFu;
    const uint32_t rbB = b & 0x00FF00FFu, gaB = (b >> 8) & 0x00FF00FFu;
    const uint32_t rb = (rbA * (256 - w) + rbB * w) >> 8 & 0x00FF00FFu;
    const uint32_t ga = (gaA * (256 - w) + gaB * w) & 0xFF00FF00u;
    return rb | ga;
}
uint32_t Weight(const Map& m, DWORD now) {
    if (!g_enabled.load(std::memory_order_relaxed)) return 256; // the option off: the new light at once (an atrium still waits)
    const float t = std::clamp(static_cast<float>(now - m.start) / static_cast<float>(kFadeMs), 0.0f, 1.0f);
    return static_cast<uint32_t>(t * t * (3.0f - 2.0f * t) * 256.0f + 0.5f); // smoothstep
}
// what is on screen now: from <- blend(from, to)
void Settle(Map& m, DWORD now) {
    const uint32_t w = Weight(m, now);
    for (size_t i = 0; i < m.from.size(); i++) m.from[i] = Mix(m.from[i], m.to[i], w);
}

HRESULT __stdcall LockHook(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    if (t_own || level != 0 || (!g_active.load(std::memory_order_relaxed) && !g_count.load(std::memory_order_relaxed)))
        return g_origLock(t, level, out, rect, flags);
    UINT w = 0, h = 0;
    // A refinement (the quick pass's room solved again at its higher class, 06/10 recording 11:50, user: "it looks right
    // for a moment, recalculates, looks wrong, then right"): the game writes the maps of that higher class, which are not
    // the ones on screen and still hold the room's light from BEFORE the edit; when the solve ends the room shows them,
    // and a fade from that content brought the old light back for a quarter of a second, room after room. Those maps take
    // the new content at once.
    const bool refining = RoomLightQueue::SolveRefiningUp();
    const void* solving = RoomLightQueue::SolvingRoom();
    std::unique_lock<std::mutex> lk(g_mx);
    auto it = g_maps.find(t);
    if (it == g_maps.end()) {
        // a map changing while a lamp edit is pending: its content as it is now (before the solve writes it)
        // only a map written by a room solve (never the UI or anything else the game updates meanwhile)
        if (refining || !g_active.load(std::memory_order_relaxed) || g_maps.size() >= kMaxMaps || !RoomLightQueue::SolveInProgress() || !RoomMap(t, w, h)) {
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
        m.to.resize(static_cast<size_t>(w) * h);
        FromMemory(m, m.to, RECT{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)}, static_cast<const BYTE*>(lr.pBits), lr.Pitch);
        g_origUnlock(t, 0);
        m.from = m.to;
        m.last = GetTickCount();
        t->AddRef();
        it = g_maps.emplace(t, std::move(m)).first;
        g_count.store(g_maps.size(), std::memory_order_relaxed);
        g_peak.store(std::max<long>(g_peak.load(), static_cast<long>(g_maps.size())), std::memory_order_relaxed);
    }
    const HRESULT hr = g_origLock(t, level, out, rect, flags);
    if (FAILED(hr) || !out || !out->pBits) return hr;
    Map& m = it->second;
    g_gameLocks.fetch_add(1, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    if (m.fading) { // what is on screen becomes the start of the next fade; the game gets its exact content
        Settle(m, now);
        m.fading = false;
        m.resume = true;
    }
    if (m.held) m.resume = true; // held: the screen shows "from", the game gets its exact content
    if (refining) m.exact = true;
    if (solving) m.room = solving;
    m.rect = Clip(m, rect);
    m.flags = flags;
    m.gameLocks++;
    if (m.resume) ToMemory(m, m.to, m.rect, static_cast<BYTE*>(out->pBits), out->Pitch);
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
            if (m.bits && !(m.flags & D3DLOCK_READONLY)) changed = FromMemory(m, m.to, m.rect, m.bits, m.pitch);
            if (m.exact) { // a refinement's map: the game's content stays (see LockHook), and later fades start from it
                if (changed || m.resume) g_exact.fetch_add(1, std::memory_order_relaxed);
                m.from = m.to;
                m.fading = m.held = m.resume = m.exact = false;
            } else if (changed || m.resume) {
                // what was on screen goes back; the frames fade it to the new content, or, while the edit's other rooms
                // are still being solved, it is held and starts with them (every story together). The fade off and
                // nothing to wait for: the game's content stays as it is.
                const bool group = m.room && !LevelLightShare::LampDragging() && LevelLightShare::GroupPending(m.room);
                const bool hold = g_holding.load(std::memory_order_relaxed) || group;
                if (!hold && !g_enabled.load(std::memory_order_relaxed)) {
                    m.from = m.to;
                    m.fading = m.held = m.groupHeld = false;
                } else if (hold) {
                    if (m.bits) ToMemory(m, m.from, m.rect, m.bits, m.pitch);
                    if (!m.held) {
                        g_heldMaps.fetch_add(1, std::memory_order_relaxed);
                        if (group) g_groupHolds.fetch_add(1, std::memory_order_relaxed);
                        m.heldAt = GetTickCount();
                    }
                    m.held = true;
                    m.groupHeld = group;
                } else {
                    if (m.bits) ToMemory(m, m.from, m.rect, m.bits, m.pitch);
                    if (!m.fading) g_fades.fetch_add(1, std::memory_order_relaxed);
                    if (m.resume && !m.held) g_resumes.fetch_add(1, std::memory_order_relaxed);
                    m.held = false;
                    m.fading = true;
                    m.start = GetTickCount();
                }
                m.resume = false;
            }
            m.last = GetTickCount();
            m.bits = nullptr;
        }
    }
    return g_origUnlock(t, level);
}

bool InstallHooks(IDirect3DDevice9* dev) {
    IDirect3DTexture9* probe = nullptr;
    if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) || !probe) return false;
    void** vt = *reinterpret_cast<void***>(probe);
    g_origLock = reinterpret_cast<LockRect_t>(vt[19]);
    g_origUnlock = reinterpret_cast<UnlockRect_t>(vt[20]);
    probe->Release();
    std::vector<DetourBatch::Hook> hooks = {{reinterpret_cast<void**>(&g_origLock), reinterpret_cast<void*>(&LockHook)},
                                            {reinterpret_cast<void**>(&g_origUnlock), reinterpret_cast<void*>(&UnlockHook)}};
    return DetourBatch::InstallHooks(hooks);
}

} // namespace

namespace RoomLightFade {

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_hookTried.exchange(true)) {
        const bool ok = dev && InstallHooks(dev);
        g_hooked.store(ok);
        LOG_INFO(ok ? "[RoomLightFade] Texture lock hooks installed: room maps fade to their new light during lamp edits"
                    : "[RoomLightFade] Texture lock hooks not available: light changes stay instant");
    }
    if (!g_hooked.load()) return;
    const bool editPending = LevelLightShare::LampEditPending();
    // maps are kept during every lamp edit, the option on or off: the option is only the fade (Weight); an atrium's stories
    // waiting for each other (kGroupHoldMs) is not optional (06/10, user: "the fade is not needed any more, is it?")
    g_active.store(editPending || LampMarkFilter::MassSwitchActive(), std::memory_order_relaxed);
    // hold while the edit's first solves run: the quick pass of many lamps, or every room of a smaller edit (not its
    // refinement, not a dragged lamp); at most kHoldMaxMs
    const DWORD now = GetTickCount();
    // 06/10, user: holding made it worse (the light's own story waited for the other stories' cascade): off until the
    // cascade itself is fast (docs/features/performance/room-light-queue.md)
    bool hold = kHoldStories && g_active.load(std::memory_order_relaxed) && !LevelLightShare::LampDragging() &&
                (RoomLightQueue::QuickPassPending() || (editPending && !RoomLightQueue::Refining()));
    if (hold && !g_holding.load(std::memory_order_relaxed)) g_holdFrom = now;
    if (hold && now - g_holdFrom > kHoldMaxMs) hold = false;
    g_holding.store(hold, std::memory_order_relaxed);
    if (!g_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!hold) { // every held map starts its fade in this frame (an atrium's: once its other stories are solved)
        bool released = false;
        for (auto& [tex, m] : g_maps) {
            if (!m.held || m.gameLocks != 0) continue;
            if (m.groupHeld && m.room) {
                if (now - m.heldAt >= kGroupHoldMs) g_groupTimeouts.fetch_add(1, std::memory_order_relaxed);
                else if (LevelLightShare::GroupPending(m.room)) continue;
            }
            m.held = false;
            m.groupHeld = false;
            m.fading = true;
            m.start = now;
            g_fades.fetch_add(1, std::memory_order_relaxed);
            released = true;
        }
        if (released) g_releases.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto it = g_maps.begin(); it != g_maps.end();) {
        Map& m = it->second;
        IDirect3DTexture9* tex = it->first;
        if (m.gameLocks > 0 || m.held) { ++it; continue; } // never written while the game holds it; held: shows "from"
        if (m.fading) {
            const uint32_t w = Weight(m, now);
            D3DLOCKED_RECT lr{};
            t_own = true;
            if (SUCCEEDED(g_origLock(tex, 0, &lr, nullptr, 0))) {
                for (UINT y = 0; y < m.h; y++) {
                    uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
                    const size_t base = static_cast<size_t>(y) * m.w;
                    if (w >= 256) std::memcpy(row, &m.to[base], m.w * 4);
                    else for (UINT x = 0; x < m.w; x++) row[x] = Mix(m.from[base + x], m.to[base + x], w);
                }
                g_origUnlock(tex, 0);
                g_writes.fetch_add(1, std::memory_order_relaxed);
            }
            t_own = false;
            if (w >= 256) {
                m.from = m.to;
                m.fading = false;
            }
            ++it;
            continue;
        }
        if (now - m.last > kKeepMs) { // idle: buffers and the reference go
            tex->Release();
            it = g_maps.erase(it);
            continue;
        }
        ++it;
    }
    g_count.store(g_maps.size(), std::memory_order_relaxed);
}

void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void Clear() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& [tex, m] : g_maps) {
        // a fade cut short: the map gets its exact content
        if ((m.fading || m.held) && m.gameLocks == 0) {
            D3DLOCKED_RECT lr{};
            t_own = true;
            if (SUCCEEDED(g_origLock(tex, 0, &lr, nullptr, 0))) {
                for (UINT y = 0; y < m.h; y++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &m.to[static_cast<size_t>(y) * m.w], m.w * 4);
                g_origUnlock(tex, 0);
            }
            t_own = false;
        }
        tex->Release();
    }
    g_maps.clear();
    g_count.store(0, std::memory_order_relaxed);
}

std::string Status() {
    if (!g_hooked.load()) return g_hookTried.load() ? "not available" : "waiting for the device";
    return std::format("fade {} | maps kept {} (at most {} at once), fades {} (restarted by a new solve {}), refinements shown at once {}, held for the other stories {} (of an atrium {}, released after the longest wait {}; released together {} times), frame writes {}, game locks seen {}",
                       g_enabled.load() ? "on" : "off", g_count.load(), g_peak.load(), g_fades.load(), g_resumes.load(), g_exact.load(), g_heldMaps.load(), g_groupHolds.load(), g_groupTimeouts.load(), g_releases.load(), g_writes.load(), g_gameLocks.load());
}

} // namespace RoomLightFade
