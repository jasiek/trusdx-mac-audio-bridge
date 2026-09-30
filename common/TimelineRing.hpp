#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace trusdx {

// Mono float ring indexed by absolute sample time on the timeline both devices
// share. One writer, any number of readers; reads never consume, so several apps
// can record the same input. Frames the writer never produced (or that are too
// old to still be in the ring) read back as silence.
class TimelineRing {
public:
    static constexpr int64_t kFrames = 1 << 16; // ~1.37 s at 48 kHz

    void Write(int64_t t, const float* src, uint32_t n)
    {
        const int64_t end = end_.load(std::memory_order_relaxed);
        if (t > end) {
            // Silence the span the writer skipped so stale audio can't resurface.
            for (int64_t f = std::max(end, t - kFrames); f < t; ++f) {
                buf_[f & kMask] = 0.0f;
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            buf_[(t + i) & kMask] = src[i];
        }
        if (t + int64_t(n) > end) {
            end_.store(t + n, std::memory_order_release);
        }
    }

    void Read(int64_t t, float* dst, uint32_t n) const
    {
        const int64_t end = end_.load(std::memory_order_acquire);
        const int64_t oldest = end - kFrames + kGuard;
        for (uint32_t i = 0; i < n; ++i) {
            const int64_t f = t + i;
            dst[i] = (f < end && f >= oldest) ? buf_[f & kMask] : 0.0f;
        }
    }

private:
    static constexpr int64_t kMask = kFrames - 1;
    static constexpr int64_t kGuard = 4096; // margin so a reader never sees half-overwritten frames

    float buf_[kFrames] = {};
    std::atomic<int64_t> end_{0};
};

} // namespace trusdx
