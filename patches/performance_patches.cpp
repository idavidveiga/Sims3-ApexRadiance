// Performance features (docs/features/performance.md):
//   ResourceLookupCache  remembers which package answers each resource lookup (features/resource_cache.h). Off by default:
//                        its invalidation was verified in the disassembly but not yet in game (use the Developer page's
//                        checks, then turn the default on).
//   LotLightingMotion    while the camera moves, scales the lot lighting budget down (features/lot_lighting_motion.h).
//   FastTextureCompression  the game's CPU DXT1 / DXT5 encoders replaced by a bit-identical faster version
//                        (features/fast_dxt.h). Off by default until checked in game.
//   FastCacheCompression the RefPack stream write answered by a faster compressor with the same stream format
//                        (features/fast_refpack.h). Off by default until checked in game.
//   SceneNodeBudget      while the camera moves, Scene::BeginFrame's pending-node drain processes at most N nodes / T ms
//                        per frame, the rest the next frames (features/scene_budget.h). Experimental, off by default.
//   ObjectLookupIndex    the object-by-ID lookup answered from a validated index of the paths the game's walk found
//                        (features/object_index.h). Experimental, off by default.
// All are drawn by the menu's Performance page (apex_gui.cpp, PerformanceCard); their developer lines go to Developer >
// Profiler.
//
// Part of Apex Radiance. Credits: @loinyx

#include "patch_base.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "performance.h"
#include "resource_cache.h"
#include "lot_lighting_motion.h"
#include "fast_dxt.h"
#include "fast_refpack.h"
#include "scene_budget.h"
#include "object_index.h"
#include <algorithm>
#include <atomic>
#include <format>

namespace {

class ResourceLookupCachePatch : public ApexPatch {
  public:
    ResourceLookupCachePatch() : ApexPatch(Performance::kResourceCacheName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!ResourceCache::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        ResourceCache::Stop();
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the rows
    void RenderDeveloperUI() override { ResourceCache::RenderDeveloperUI(); }
};

class LotLightingMotionPatch;
std::atomic<LotLightingMotionPatch*> g_lotPatch{nullptr};

class LotLightingMotionPatch : public ApexPatch {
  public:
    LotLightingMotionPatch() : ApexPatch(Performance::kLotLightingName, nullptr) {
        // TOML key: never rename
        RegisterIntSetting(&budgetMs_, "budgetWhileMovingMs", Performance::kLotLightingBudgetDefault, 1, 15, "Lot lighting time while moving (ms)");
        g_lotPatch.store(this);
    }
    ~LotLightingMotionPatch() override { g_lotPatch.store(nullptr); }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        LotLightingMotion::SetBudgetMs(budgetMs_);
        std::string error;
        if (!LotLightingMotion::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        LotLightingMotion::Stop();
        if (LotLightingMotion::Running()) return Fail("Could not put the lot lighting call back (see ApexRadiance_LOG.txt)");
        isEnabled = false;
        lastError.clear();
        return true;
    }

    // The budget is read by the hook on every call: applied at once, nothing to reinstall
    void Update() override {
        pendingReinstall = false;
        LotLightingMotion::SetBudgetMs(budgetMs_);
    }

    void RenderCustomUI() override {} // the Performance card draws the rows
    void RenderDeveloperUI() override { LotLightingMotion::RenderDeveloperUI(); }

    int BudgetMs() const { return budgetMs_; }
    void SetBudgetMs(int ms) {
        ms = std::clamp(ms, 1, 15);
        if (ms == budgetMs_) return;
        budgetMs_ = ms;
        LotLightingMotion::SetBudgetMs(ms);
        NotifySettingChanged(); // saved (Update clears the reinstall request)
    }

  private:
    int budgetMs_ = Performance::kLotLightingBudgetDefault;
};

class FastTextureCompressionPatch : public ApexPatch {
  public:
    FastTextureCompressionPatch() : ApexPatch(Performance::kFastTextureName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!FastDxt::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        const bool restored = FastDxt::Stop(); // off either way: a hook that could not be removed passes every call through
        isEnabled = false;
        lastError = restored ? std::string() : std::string("An encoder entry could not be put back; the game's encoder runs through it (see ApexRadiance_LOG.txt)");
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the row
    void RenderDeveloperUI() override { FastDxt::RenderDeveloperUI(); }
};

class FastCacheCompressionPatch : public ApexPatch {
  public:
    FastCacheCompressionPatch() : ApexPatch(Performance::kFastCacheName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!FastRefPack::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        FastRefPack::Stop();
        isEnabled = false;
        lastError.clear();
        return true;
    }

    // Removes the vtable layer a moment after the feature was turned off (see FastRefPack::Tick); nothing to reinstall
    void Update() override {
        pendingReinstall = false;
        FastRefPack::Tick();
    }

    void RenderCustomUI() override {} // the Performance card draws the row
    void RenderDeveloperUI() override { FastRefPack::RenderDeveloperUI(); }
};

class SceneNodeBudgetPatch : public ApexPatch {
  public:
    SceneNodeBudgetPatch() : ApexPatch(Performance::kSceneBudgetName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!SceneBudget::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        SceneBudget::Stop(); // the hook runs the game's drain from then on, even if the CALL could not be put back
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the row
    void RenderDeveloperUI() override { SceneBudget::RenderDeveloperUI(); }
};

class ObjectLookupIndexPatch : public ApexPatch {
  public:
    ObjectLookupIndexPatch() : ApexPatch(Performance::kObjectIndexName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!ObjectIndex::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        ObjectIndex::Stop(); // the layer passes every call to the game from then on, even if the entry could not be put back
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the row
    void RenderDeveloperUI() override { ObjectIndex::RenderDeveloperUI(); }
};

} // namespace

int Performance::LotLightingBudgetMs() {
    LotLightingMotionPatch* p = g_lotPatch.load();
    return p ? p->BudgetMs() : kLotLightingBudgetDefault;
}

void Performance::SetLotLightingBudgetMs(int ms) {
    if (LotLightingMotionPatch* p = g_lotPatch.load()) p->SetBudgetMs(ms);
}

std::string Performance::ResourceCacheStatus() { return ResourceCache::StatusText(); }
std::string Performance::LotLightingStatus() { return LotLightingMotion::StatusText(); }
std::string Performance::FastTextureStatus() { return FastDxt::StatusText(); }
std::string Performance::FastCacheStatus() { return FastRefPack::StatusText(); }
std::string Performance::SceneBudgetStatus() { return SceneBudget::StatusText(); }
std::string Performance::ObjectIndexStatus() { return ObjectIndex::StatusText(); }

APEX_REGISTER_FEATURE(ResourceLookupCachePatch,
                      {.displayName = "Faster Game File Lookups",
                       .description = "Remembers which of the game's packages holds each file the game asks for, so it does not search every package again. Fewer small "
                                      "stutters when objects, textures and lots load. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"ResourceMgr::FindProvider (0x4AFFC0) answers from a table keyed by (manager, resource key); each answer is re-checked with one probe of "
                                            "the package that holds it plus one per package above it that can gain files.",
                                            "Emptied on every package-list change (RegisterDatabase 0x4B2D00 / 0x736A70, SetDatabasePriority 0x4B2EC0) and on the "
                                            "engine's own change notice (0x4B0960). Vtable slots only; the Frame Profiler keeps counting."},
                       .gameCodeGroup = "ResourceCache"});

APEX_REGISTER_FEATURE(LotLightingMotionPatch,
                      {.displayName = "Lot Lighting While Moving",
                       .description = "While the camera moves, lots relight in smaller steps each frame instead of taking up to 15 ms at once, so panning over busy "
                                      "neighborhoods stutters less. Lights finish as soon as the camera stops. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = false,
                       .enabledByDefault = true,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"The lot lighting update's budget call (0xADB95D -> 0xADB120) goes through Apex: while the camera eye moved in the last 300 ms, "
                                            "the budget is scaled so the current lot gets the chosen ms and every other lot the same fraction of its own.",
                                            "Tool mode (1000 ms) is never changed; nothing is skipped, the room solves resume next frame."},
                       .gameCodeGroup = "LotLightingMotion"});

APEX_REGISTER_FEATURE(FastTextureCompressionPatch,
                      {.displayName = "Faster Texture Compression",
                       .description = "Compresses the textures the game builds while you play (terrain, Sims, lot views, thumbnails) several times faster, with "
                                      "exactly the same result, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"The CPU DXT1 / DXT5 encoders (0x6152F0 / 0x6154B0) are replaced at their entries by the same algorithm run on four blocks at "
                                            "once with SSE2: every block gets the same bytes as the game's.",
                                            "The first 16 textures of each session are also encoded by the game and compared; a difference turns the feature off."},
                       .gameCodeGroup = "FastTextureCompression"});

APEX_REGISTER_FEATURE(FastCacheCompressionPatch,
                      {.displayName = "Faster Cache Compression",
                       .description = "Compresses what the game stores in its caches and saves (Sims, objects, terrain) with a much faster compressor in the game's own "
                                      "format, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"The RefPack stream write (0x4EC200) is answered through its vtable slot by a bounded hash-chain compressor with reusable "
                                            "memory; same header, window and opcodes, so the game's decoder reads it unchanged.",
                                            "The first 16 streams of each session are decompressed with the game's decoder and compared with the source; a difference "
                                            "turns the feature off."},
                       .gameCodeGroup = "FastCacheCompression"});

APEX_REGISTER_FEATURE(SceneNodeBudgetPatch,
                      {.displayName = "Spread New Objects Over Frames",
                       .description = "While the camera moves, objects that just loaded or moved are placed in the scene a few hundred per frame instead of all "
                                      "at once, so panning over a lot that streams in stutters less. An object may appear a frame or two later; everything is "
                                      "placed at once as soon as the camera stops. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"Scene::BeginFrame's call of the pending-node drain (0x6EBC49 -> 0x6E4130) goes through Apex: while the camera eye moved in "
                                            "the last 300 ms, an exact copy of the game's loop stops after 512 nodes or 2 ms; the rest stays queued in the game's own "
                                            "list and goes first next frame.",
                                            "Camera still, or a node waited 500 ms: the game's own drain runs. The other five callers of the drain are untouched."},
                       .gameCodeGroup = "SceneNodeBudget"});

APEX_REGISTER_FEATURE(ObjectLookupIndexPatch,
                      {.displayName = "Faster Object Lookups",
                       .description = "Remembers where the game found each lot when it looks one up by its ID, instead of searching the whole world every time. "
                                      "Fewer stutters when lot lights update and less work for the game's scripts. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"The lookup by ID (0xC62D40, a depth-first walk of the world's object tree) answers from a table of the paths the game's walk "
                                            "found; every answer re-reads its path in the live tree (indices, classes, the ID) and walks again on any difference.",
                                            "The first 64 answers of each session and then 1 in 64 are checked against the game's walk; a difference turns the feature "
                                            "off."},
                       .gameCodeGroup = "ObjectIndex"});
