// Performance features (docs/features/performance.md):
//   ResourceLookupCache  remembers which package answers each resource lookup (features/resource_cache.h). Off by default:
//                        its invalidation was verified in the disassembly but not yet in game (use the Developer page's
//                        checks, then turn the default on).
//   ResourceLookupMisses "Remember missing files": the cache also keeps "no package holds it" answers, and counts the
//                        writes of the traced database classes so unchanged packages need no re-check. Off by default
//                        until checked in game; idle while ResourceLookupCache is off.
//   FileListCache        the GetKeyList cache for the key-type filter (same module). Off by default until checked in game.
//   LotLightingMotion    while the camera moves, scales the lot lighting budget down (features/lot_lighting_motion.h).
//   WallShadingWhileMoving  defers the wall ambient-occlusion pass while the camera moves and allows one pass per frame
//                        (features/lot_lighting_motion.h). On by default: it only returns the engine's own "try later".
//   FastTextureCompression  the game's CPU DXT1 / DXT5 encoders replaced by a bit-identical faster version
//                        (features/fast_dxt.h). Off by default until checked in game.
//   FastCacheCompression the RefPack stream write answered by a faster compressor with the same stream format
//                        (features/fast_refpack.h). Off by default until checked in game.
//   SceneNodeBudget      while the camera moves, Scene::BeginFrame's pending-node drain processes at most N nodes / T ms
//                        per frame, the rest the next frames; the nodes left are guarded by hooks on the node destructor,
//                        AddNode and the holder teardown (features/scene_budget.h). Experimental, off by default.
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
#include "room_light_queue.h"
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

// "Remember missing files": an extension of the lookup cache (idle while the cache is off)
class ResourceLookupMissesPatch : public ApexPatch {
  public:
    ResourceLookupMissesPatch() : ApexPatch(Performance::kLookupMissesName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!ResourceCache::StartMisses(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        ResourceCache::StopMisses();
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {}   // the Performance card draws the row
    void RenderDeveloperUI() override {} // the lookup cache's developer lines include it
};

class FileListCachePatch : public ApexPatch {
  public:
    FileListCachePatch() : ApexPatch(Performance::kFileListName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!ResourceCache::StartKeyLists(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        ResourceCache::StopKeyLists();
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {}   // the Performance card draws the row
    void RenderDeveloperUI() override {} // the lookup cache's developer lines include it
};

class WallShadingWhileMovingPatch : public ApexPatch {
  public:
    WallShadingWhileMovingPatch() : ApexPatch(Performance::kWallShadingName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!LotLightingMotion::StartWallAo(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        LotLightingMotion::StopWallAo(); // the gate layer passes every call through from now on
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the row
    void RenderDeveloperUI() override { LotLightingMotion::RenderWallAoDeveloperUI(); }
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

class FastTextureCompressionPatch;
std::atomic<FastTextureCompressionPatch*> g_texPatch{nullptr};

class FastTextureCompressionPatch : public ApexPatch {
  public:
    FastTextureCompressionPatch() : ApexPatch(Performance::kFastTextureName, nullptr) {
        // TOML key: never rename
        RegisterBoolSetting(&severalCores_, "useSeveralCores", true, "Use several cores");
        g_texPatch.store(this);
    }
    ~FastTextureCompressionPatch() override { g_texPatch.store(nullptr); }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        FastDxt::SetSeveralCores(severalCores_);
        std::string error;
        if (!FastDxt::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    // "Use several cores" is read by the hook on every image: applied at once, nothing to reinstall
    void Update() override {
        pendingReinstall = false;
        FastDxt::SetSeveralCores(severalCores_);
    }

    bool SeveralCores() const { return severalCores_; }
    void SetSeveralCores(bool on) {
        if (on == severalCores_) return;
        severalCores_ = on;
        FastDxt::SetSeveralCores(on);
        NotifySettingChanged(); // saved (Update clears the reinstall request)
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        const bool restored = FastDxt::Stop(); // off either way: a hook that could not be removed passes every call through
        isEnabled = false;
        lastError = restored ? std::string() : std::string("An encoder entry could not be put back; the game's encoder runs through it (see ApexRadiance_LOG.txt)");
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the rows
    void RenderDeveloperUI() override { FastDxt::RenderDeveloperUI(); }

  private:
    bool severalCores_ = true;
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

class RoomLightQueuePatch : public ApexPatch {
  public:
    RoomLightQueuePatch() : ApexPatch(Performance::kRoomLightQueueName, nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        if (!RoomLightQueue::Start(&error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        RoomLightQueue::Stop();
        if (RoomLightQueue::Running()) return Fail("Could not put the room lighting code back (see ApexRadiance_LOG.txt)");
        isEnabled = false;
        lastError.clear();
        return true;
    }

    void RenderCustomUI() override {} // the Performance card draws the row
    void RenderDeveloperUI() override { RoomLightQueue::RenderDeveloperUI(); }
};

} // namespace

int Performance::LotLightingBudgetMs() {
    LotLightingMotionPatch* p = g_lotPatch.load();
    return p ? p->BudgetMs() : kLotLightingBudgetDefault;
}

void Performance::SetLotLightingBudgetMs(int ms) {
    if (LotLightingMotionPatch* p = g_lotPatch.load()) p->SetBudgetMs(ms);
}

bool Performance::FastTextureSeveralCores() {
    FastTextureCompressionPatch* p = g_texPatch.load();
    return p ? p->SeveralCores() : true;
}

void Performance::SetFastTextureSeveralCores(bool on) {
    if (FastTextureCompressionPatch* p = g_texPatch.load()) p->SetSeveralCores(on);
}

std::string Performance::ResourceCacheStatus() { return ResourceCache::StatusText(); }
std::string Performance::LookupMissesStatus() { return ResourceCache::MissesStatusText(); }
std::string Performance::FileListStatus() { return ResourceCache::KeyListStatusText(); }
std::string Performance::LotLightingStatus() { return LotLightingMotion::StatusText(); }
std::string Performance::WallShadingStatus() { return LotLightingMotion::WallAoStatusText(); }
std::string Performance::FastTextureStatus() { return FastDxt::StatusText(); }
std::string Performance::FastCacheStatus() { return FastRefPack::StatusText(); }
std::string Performance::SceneBudgetStatus() { return SceneBudget::StatusText(); }
std::string Performance::ObjectIndexStatus() { return ObjectIndex::StatusText(); }
std::string Performance::RoomLightQueueStatus() { return RoomLightQueue::StatusText(); }

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

APEX_REGISTER_FEATURE(ResourceLookupMissesPatch,
                      {.displayName = "Remember Missing Files",
                       .description = "Lets Faster Game File Lookups also remember files that no package has, so the game does not search every package for them again "
                                      "and again. Needs Faster Game File Lookups. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"FindProvider answers of 0 (about a third of all lookups: the resolve 0x7D8110 retries every miss with the group bit flipped) "
                                            "are kept too, re-checked against every package that can gain files; stored only when every read-only package answered "
                                            "from its key set or open index.",
                                            "Write epochs: the write paths of the DPF, DDF and packed stream classes are hooked (vtable slots, plus the DPF direct write "
                                            "0x4A7FC0), so answers whose packages did not change are not re-checked at all; other classes are still probed."},
                       .gameCodeGroup = "ResourceCache"});

APEX_REGISTER_FEATURE(FileListCachePatch,
                      {.displayName = "Faster File Lists",
                       .description = "Remembers which files of a kind each of the game's packages holds, so Create a Sim and Sim loading do not read the list of every "
                                      "package again. Fewer small stutters when Sims change outfits or load. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = false,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"ResourceMgr::GetKeyList (0x4B1AE0 / 0x736660, vtable slots) for the key-type filter: each read-only package's matching keys "
                                            "are kept per type and appended in the same package order; every other package is asked each time.",
                                            "Emptied on every package-list change and change notice, like the lookup cache."},
                       .gameCodeGroup = "FileListCache"});

APEX_REGISTER_FEATURE(WallShadingWhileMovingPatch,
                      {.displayName = "Wall Shading While Moving",
                       .description = "While the camera moves, the soft ambient shading of the outdoor walls of newly loaded lots waits until the camera stops (or "
                                      "two seconds), and never more than one wall pass runs per frame, so panning over a neighborhood that is loading stutters "
                                      "less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                       .category = "Performance",
                       .experimental = false,
                       .enabledByDefault = true,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"The wall ambient-occlusion step (0x68B810, vtable slot 0xFF05B0) shades every outdoor wall of a lot level with no time check. "
                                            "Called from its solver driver (0x688920) with a frame budget, Apex may return the current state instead, which the driver "
                                            "stores unchanged: the engine's own \"try again next frame\".",
                                            "The synchronous level solve (60 s budget) and the tool mode are never touched."},
                       .gameCodeGroup = "WallShadingWhileMoving"});

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
                                            "Use several cores: textures of 256 x 256 and more are split by rows of blocks over up to 6 worker threads (the calling "
                                            "thread's floating-point state in each); the call still returns the finished texture.",
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
                                            "Camera still, or a node waited 500 ms: the game's own drain runs. The other five callers of the drain are untouched.",
                                            "Every node left queued is recorded: the node destructor (0x6FD930) unlinks a recorded node that is still queued, AddNode "
                                            "(0x6E6480) unlinks one before queueing it again, and the scene teardown (0x6E4DE0) forgets its records, so a node "
                                            "held for later can never be freed or queued twice while linked."},
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

APEX_REGISTER_FEATURE(RoomLightQueuePatch,
                      {.displayName = "Faster Room Lighting",
                       .description = "Rooms light up much sooner when you enter a lot, change floors or switch lamps: the lot you are on and the floor you look at "
                                      "go first, rooms reach their final look in fewer steps, and several small rooms are lit per frame. Part of " APEX_PRODUCT_NAME ". "
                                      "Credits: @loinyx",
                       .category = "Performance",
                       .experimental = true,
                       .enabledByDefault = true,
                       .supportedVersions = VERSION_STEAM,
                       .technicalDetails = {"The game relights rooms one at a time for the whole world, one per frame at most: the priority of each room (CALL 0x6A81DF "
                                            "-> 0x69E770) is raised for the priority lot on and below the camera's story; a finished class-0 solve steps straight to "
                                            "the target class (0x69EAA2); an invalidated room keeps its class (0x69EF58, 0x69F1C5).",
                                            "After the scheduler (jmp 0x6C5E39 -> 0x6C5C20), rooms of the priority lot are solved at once and the next one picked, within "
                                            "4 ms per frame (1 ms while the camera moves), on the render thread only."},
                       .gameCodeGroup = "RoomLightQueue"});
