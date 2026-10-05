#pragma once
#include <d3d9.h>

// Alias the game's map on a spare sampler without changing its filtering or LOD.
// Setter goes below Apex's texture hook. Partial failures are restored too.
template<class Device, class Setter> class NativeTerrainSampler {
    Device* dev;
    Setter setter;
    DWORD slot;
    IDirect3DBaseTexture9* old = nullptr;
    DWORD saved[11]{};
    unsigned changed = 0;
    bool textureChanged = false, ready = false;
public:
    NativeTerrainSampler(Device* d, DWORD source, DWORD extra, IDirect3DBaseTexture9* native, Setter set)
        : dev(d), setter(set), slot(extra) {
        if (!native || source == extra || FAILED(dev->GetTexture(slot, &old))) return;
        for (DWORD i = 0; i < 11; ++i) {
            const auto state = static_cast<D3DSAMPLERSTATETYPE>(i + 1);
            DWORD value = 0;
            if (FAILED(dev->GetSamplerState(slot, state, &saved[i])) || FAILED(dev->GetSamplerState(source, state, &value))) return;
            if (value != saved[i]) {
                if (FAILED(dev->SetSamplerState(slot, state, value))) return;
                changed |= 1u << i;
            }
        }
        if (old != native) {
            if (FAILED(setter(dev, slot, native))) return;
            textureChanged = true;
        }
        ready = true;
    }
    ~NativeTerrainSampler() {
        if (textureChanged) setter(dev, slot, old);
        for (int i = 10; i >= 0; --i)
            if (changed & (1u << i)) dev->SetSamplerState(slot, static_cast<D3DSAMPLERSTATETYPE>(i + 1), saved[i]);
        if (old) old->Release();
    }
    explicit operator bool() const { return ready; }
    NativeTerrainSampler(const NativeTerrainSampler&) = delete;
    NativeTerrainSampler& operator=(const NativeTerrainSampler&) = delete;
};
