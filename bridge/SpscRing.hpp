#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace trusdx {

// Lock-free single-producer / single-consumer FIFO. Safe to use from a
// Core Audio I/O thread: no locks, no allocation after construction.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2)
        : buf_(capacityPow2)
        , mask_(capacityPow2 - 1)
    {
    }

    // Producer side. Returns how many items fit; the rest are dropped.
    size_t Push(const T* src, size_t n)
    {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        n = std::min(n, buf_.size() - (w - r));
        for (size_t i = 0; i < n; ++i) {
            buf_[(w + i) & mask_] = src[i];
        }
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer side.
    size_t Pop(T* dst, size_t n)
    {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        for (size_t i = 0; i < n; ++i) {
            dst[i] = buf_[(r + i) & mask_];
        }
        r_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer side.
    void Clear()
    {
        r_.store(w_.load(std::memory_order_acquire), std::memory_order_release);
    }

    size_t Size() const
    {
        return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire);
    }

private:
    std::vector<T> buf_;
    const size_t mask_;
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
};

} // namespace trusdx
