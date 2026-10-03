#pragma once
#include <algorithm>
#include <chrono>

namespace Overlay {
// Sample every game frame, including frames with no overlay. Reopening the menu must not
// feed the entire time it was closed into ImGui's animations and FPS average.
class FrameClock {
  public:
    using Clock = std::chrono::steady_clock;
    float Step(Clock::time_point now) {
        const float elapsed = havePrevious ? std::chrono::duration<float>(now - previous).count() : 1.0f / 60.0f;
        previous = now;
        havePrevious = true;
        return std::max(elapsed, 0.000001f);
    }
  private:
    Clock::time_point previous{};
    bool havePrevious = false;
};
} // namespace Overlay
