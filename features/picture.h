#pragma once
// Picture filters (Apex Radiance): colour and image filters for the game's 3D scene, applied at the
// end of the frame on the normal 8-bit back buffer. The interface (the game's UI and the Apex menu) keeps its own
// colours: the back buffer is copied at the point where the game goes from the scene to its UI, and pixels that
// changed after that copy are left as they are.
//
// The pass: gamma 2.2 decode, gradient smoothing (deband), sharpening, clarity (local contrast against a 1/8-size copy
// of the scene), exposure, white balance, contrast, midtones / shadows / highlights / blacks, split toning, vibrance,
// saturation with a per-hue colour mixer, vignette, then gamma 2.2 encode with a fixed dither below one 8-bit step so the
// grading adds no banding. Before/after compare shows the left half unprocessed.
// Settings: [qol.picture] in ApexRadiance.toml (same keys as the combined build).
//
// Extracted from the combined build's hdr_output.cpp (SDR path only; the HDR output was removed from the standalone).
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <mutex>
#include <string>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

struct PictureParams {
    bool enabled = false;
    float exposure = 0.0f;    // scene brightness in stops (EV)
    float contrast = 1.0f;    // around mid grey (0.18 of white)
    float midtones = 1.0f;    // > 1 brighter midtones (black and white stay)
    float shadows = 0.0f;     // -1..1
    float highlights = 0.0f;  // -1..1
    float blacks = 0.0f;      // -1 lifts the blacks, +1 deepens them (2% of white at the ends)
    float temperature = 0.0f; // -1 cooler (bluer) .. +1 warmer
    float tint = 0.0f;        // -1 greener .. +1 more magenta
    float saturation = 1.0f;
    float vibrance = 0.0f;    // -1..1
    float shadowHue = 215.0f; // split toning: colour of the shadows (hue in degrees) and how much
    float shadowTint = 0.0f;
    float highlightHue = 40.0f; // ... and of the highlights
    float highlightTint = 0.0f;
    float mixer[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // saturation per hue: red, yellow, green, cyan, blue, magenta
    float deband = 1.0f;      // gradient smoothing: 0 = off, 1 = steps up to 6/255, 2 = up to 12/255
    float sharpen = 0.0f;     // 0..1.5
    float clarity = 0.0f;     // local contrast of the midtones, -1..1
    float vignette = 0.0f;    // darker corners, 0..0.8
    float vignetteSize = 0.5f; // where the darkening starts (0 = centre, 1 = corners)
    bool compare = false;     // before/after: the left half unprocessed (not saved)

    // ---- Color > Filters: stackable looks in the same pass, each its own switch (all off by default) ----
    // Color looks
    bool tech1 = false;                       // two-strip film: a red record and a cyan record
    float tech1Amount = 0.6f, tech1Cyan = 0.0f, tech1Saturation = 1.0f; // cyan: -1 greener .. +1 bluer
    bool tech2 = false;                       // three-strip dye transfer: dense, pure primaries
    float tech2Amount = 0.5f, tech2Saturation = 1.0f, tech2Brightness = 0.0f;
    float tech2Dye[3] = {1.0f, 1.0f, 1.0f};   // strength of the red, green and blue dye
    bool dpx = false;                         // cinema negative: an S curve per channel
    float dpxAmount = 0.5f, dpxContrast = 0.5f, dpxSaturation = 1.0f;
    float dpxCurve[3] = {1.0f, 1.0f, 1.0f};   // contrast of the red, green and blue curve
    bool colourful = false;                   // livelier colors, brightest ones protected
    float colourfulAmount = 0.4f, colourfulProtect = 0.7f; // amount -1 (muted) .. +1
    bool night = false;                       // cooler, darker evening tone; lamp light kept
    float nightAmount = 0.6f, nightDarkness = 0.35f, nightBlue = 0.5f, nightKeepLamps = 0.6f;
    bool vintage = false;                     // faded photo: lifted blacks, warm, washed-out colors
    float vintageAmount = 0.7f, vintageFade = 0.5f, vintageWarmth = 0.5f, vintageColors = 0.4f;
    bool crossProcess = false;                // slide film in negative chemistry: green shadows, yellow highlights
    float crossAmount = 0.5f, crossContrast = 0.5f;
    bool bw = false;                          // black and white with a lens filter and a toning
    float bwAmount = 1.0f, bwFilterHue = 30.0f, bwFilter = 0.5f, bwToneHue = 35.0f, bwTone = 0.0f, bwContrast = 0.0f;
    bool filmic = false;                      // filmic pass: S-curve contrast, film curves per channel, bleach bypass, fade
    float filmicAmount = 0.85f, filmicFade = 0.4f, filmicContrast = 1.0f, filmicBleach = 0.0f, filmicSaturation = -0.15f;
    float filmicCurve[3] = {1.0f, 1.0f, 1.0f}; // brightness curve of the red, green and blue channel
    bool tintFilter = false;                  // the picture in one color (sepia by default), mixed in
    float tintFilterHue = 35.0f, tintFilterAmount = 0.58f;
    bool levels = false;                      // new black and white points, like an editor's Levels
    float levelsBlack = 16.0f / 255.0f, levelsWhite = 235.0f / 255.0f;
    // Light
    bool glow = false;                        // soft halo around bright areas: lamps, windows, sky
    float glowAmount = 0.4f, glowThreshold = 0.6f, glowSize = 0.5f, glowWarmth = 0.0f;
    bool halation = false;                    // film's red halo around strong light
    float halationAmount = 0.4f, halationThreshold = 0.7f, halationHue = 15.0f;
    bool dreamy = false;                      // Orton: a soft glow over the whole picture, a little more color
    float dreamyAmount = 0.4f, dreamySoftness = 0.6f, dreamySaturation = 0.3f;
    bool fakeHdr = false;                     // local contrast: detail in shadows and highlights without halos
    float hdrAmount = 0.5f, hdrRadius = 0.5f, hdrShadows = 0.4f, hdrHighlights = 0.4f, hdrHalo = 0.6f, hdrSaturation = 0.1f;
    // Camera
    bool emphasize = false;                   // grey outside a band of distance around the focus
    bool emphAuto = true;                     // focus on what is at the center of the screen
    float emphAmount = 0.8f, emphDistance = 30.0f, emphWidth = 0.5f, emphSoftness = 0.5f, emphGrey = 0.5f; // width: fraction of the distance
    bool tiltShift = false;                   // miniature: sharp band, blurred top and bottom
    float tiltAmount = 0.7f, tiltCenter = 0.55f, tiltWidth = 0.25f, tiltSaturation = 0.25f;
    bool prism = false;                       // chromatic aberration growing toward the edges
    float prismAmount = 0.35f, prismStart = 0.35f, prismQuality = 0.5f;
    bool grain = false;                       // film grain
    float grainAmount = 0.3f, grainSize = 0.3f, grainShadows = 0.5f;
    // Retro and style
    bool retro3dfx = false;                   // late-90s 3D card: 16-bit color, dithering, scanlines, soft pixels
    float fxAmount = 1.0f, fxDepth = 0.5f, fxScanlines = 0.3f, fxDither = 0.6f, fxPixelWidth = 0.3f, fxGamma = 1.0f;
    bool crt = false;                         // old TV: curved glass, phosphor mask, scanlines
    float crtAmount = 1.0f, crtCurvature = 0.3f, crtMask = 0.4f, crtScanlines = 0.4f, crtEdges = 0.4f;
};

class Picture {
  public:
    static Picture& Get() {
        static Picture instance;
        return instance;
    }

    // D3D9 bootstrap: around IDirect3DDevice9::Reset (the pass's D3DPOOL_DEFAULT targets are recreated lazily)
    void BeforeReset();
    // End of the frame, before the Apex overlay: when the frame ended on the scene (no game UI after it), the scene copy
    // is taken now.
    void BeforeOverlay(IDirect3DDevice9* dev);
    // End of the frame, after the Apex overlay (inside the game's EndScene): the filter pass, once per frame.
    void OnEndScene(IDirect3DDevice9* dev);

    PictureParams GetParams() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_p;
    }
    void SetParams(const PictureParams& p, bool save);
    float GpuMs() const { return m_gpuMs; }

    // Hold to compare (menu: the eye button, or B over the menu): while it is called every frame, the pass is skipped
    // and the game shows its original picture. It lapses by itself about 0.15 s after the last call (never saved).
    void HoldBypass();

    void SaveToToml(toml::table& qolTable) const; // [qol.picture]
    void LoadFromToml(const toml::table& qolTable);
    // The same [qol.picture] table for any parameters (looks, profiles, undo); FromToml: false when qolTable has no
    // picture table (out untouched). compare is not saved (FromToml leaves it false).
    static void ParamsToToml(const PictureParams& p, toml::table& qolTable);
    static bool ParamsFromToml(const toml::table& qolTable, PictureParams& out);
    // Keys of [qol.picture] this build reads (config migration copies only these)
    static const char* const* Keys(size_t& count);

    // Tabs of the Color page (menu: Image > Color), in order
    enum Tab : int { TabBasic, TabTones, TabColor, TabDetail, TabFilters, TabCount };
    // The Filters tab: one card per filter (its switch in the header, its own controls under it), in four sections
    void RenderFiltersUI();
    // The rows of one tab of the Color page (inside a card the menu opens), then "Reset Picture". The menu draws the
    // Picture card header with the on/off switch ([qol.picture] enabled) and the before / after button (compare) above
    // the tabs itself. The rows stay visible, greyed out, while Picture is off.
    void RenderUI(int tab);
    // Developer page > Debug views: the 8-bit / dither note and the GPU cost
    void RenderDeveloperUI();
    // Picture is on but has not been applied for 2 s: why (empty when it runs, is off, or was just turned on).
    // translated: in the menu's language (the Color card's note); else English (the log). The first time a reason shows
    // it is also logged ([Picture] On, but not applied ...).
    std::string Problem(bool translated);
    // Picture runs, but for 2 s there was no copy of the scene before the game's UI (the game did not draw its scene straight
    // into the back buffer, e.g. its own Edge Smoothing is on): the whole picture is filtered, the game's menus included
    bool MenusTinted() const;

  private:
    Picture() = default;
    void ReleaseResources();
    bool InitResources(IDirect3DDevice9* dev);

    mutable std::mutex m_mutex;
    PictureParams m_p;
    float m_gpuMs = -1.0f;
    std::atomic<unsigned long long> m_holdUntil{0}; // GetTickCount64 until which the pass is skipped (hold to compare)
    // Diagnostics (Problem): when it was turned on, when the game's EndScene last reached it, when the pass last ran,
    // why it last returned early (Skip in picture.cpp), what the resource creation failed on
    std::atomic<unsigned long long> m_enabledAt{0}, m_lastEndScene{0}, m_lastApplied{0}, m_lastSceneCopy{0};
    bool m_menusTintedLogged = false; // render thread
    std::atomic<int> m_checkPasses{0};  // passes left whose state is read back (SetParams: the next 3)
    std::string m_lastCheck;            // render thread: the last check's result (logged when it changes)
    // Status line every minute while on (render thread): passes, passes with the scene copy, the device the frames end on
    unsigned m_passes = 0, m_passesWithScene = 0;
    unsigned long long m_lastStatus = 0;
    const void* m_lastDevice = nullptr;
    int m_deviceChanges = 0;
    std::atomic<int> m_skip{0};
    std::string m_resourceError; // guarded by m_mutex
    std::string m_loggedProblem;          // render thread
    bool m_appliedLogged = false;         // render thread
    std::atomic<bool> m_resetDiag{false}; // turned on or off (any thread): the render thread starts the diagnostics over
};
