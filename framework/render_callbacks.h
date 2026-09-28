#pragma once
// Callback lists fired by the D3D9 bootstrap at points the D3D9Hooks registry does not cover: the end of the game's
// frame before the Apex overlay draws, and around IDirect3DDevice9::Reset (features release and recreate their
// D3DPOOL_DEFAULT resources there). The lists grow as needed (the first version had 4 fixed slots and dropped a fifth
// preReset user silently, so its resources survived the Reset and made it fail).
#include <d3d9.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include "apex_log.h"

namespace RenderCallbacks {

using DeviceFn = void (*)(IDirect3DDevice9*);

class CallbackList {
  public:
    explicit CallbackList(const char* name) : name_(name) {}

    void Add(DeviceFn fn) {
        if (!fn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(fns_.begin(), fns_.end(), fn) != fns_.end()) return;
        fns_.push_back(fn);
        LOG_DEBUG(std::string("[RenderCallbacks] ") + name_ + ": " + std::to_string(fns_.size()) + " callbacks");
    }

    void Remove(DeviceFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        fns_.erase(std::remove(fns_.begin(), fns_.end(), fn), fns_.end());
    }

    // Calls every callback, in the order they were added. The list is copied first: a callback may add or remove.
    void Fire(IDirect3DDevice9* device) {
        std::vector<DeviceFn> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fns_.empty()) return;
            run = fns_;
        }
        for (DeviceFn fn : run) fn(device);
    }

  private:
    std::mutex mutex_;
    std::vector<DeviceFn> fns_;
    const char* name_;
};

inline CallbackList endSceneBeforeOverlay{"endSceneBeforeOverlay"};
inline CallbackList preReset{"preReset"};
inline CallbackList postReset{"postReset"};

inline void Add(CallbackList& list, DeviceFn fn) { list.Add(fn); }
inline void Remove(CallbackList& list, DeviceFn fn) { list.Remove(fn); }
inline void Fire(CallbackList& list, IDirect3DDevice9* device) { list.Fire(device); }

} // namespace RenderCallbacks
