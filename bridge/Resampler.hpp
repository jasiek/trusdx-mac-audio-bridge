#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace trusdx {

// Windowed-sinc resampler with a continuously adjustable ratio.
//
// `step` is input samples consumed per output sample (in_rate / out_rate), so
// it can be nudged at runtime to track a drifting clock. The kernel is a
// Kaiser-windowed sinc sampled at kPhases fractional offsets, linearly
// interpolated between phases. When downsampling, the kernel is widened so
// the cutoff follows the output Nyquist rate.
//
// Realtime-safe after construction as long as fewer than kCapacity input
// samples are buffered at once (extra samples are dropped).
class Resampler {
public:
    Resampler(double step, double cutoff = 0.9, int halfTaps = 16)
        : step_(step)
    {
        const double scale = std::min(1.0, 1.0 / step); // < 1 when downsampling
        const double fc = cutoff * scale;               // relative to input Nyquist
        width_ = int(std::ceil(halfTaps / scale));
        const int taps = 2 * width_;

        table_.resize(size_t(kPhases + 1) * taps);
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = double(p) / kPhases;
            for (int j = 0; j < taps; ++j) {
                const double x = (j - (width_ - 1)) - frac;
                table_[size_t(p) * taps + j] = float(fc * Sinc(fc * x) * Kaiser(x / width_));
            }
        }
        buf_.reserve(kCapacity);
        Reset();
    }

    void SetStep(double step) { step_ = step; }
    double Step() const { return step_; }

    void Reset()
    {
        buf_.assign(size_t(width_ - 1), 0.0f);
        pos_ = width_ - 1;
    }

    void Push(float x)
    {
        if (buf_.size() < buf_.capacity()) {
            buf_.push_back(x);
        }
    }

    // Produces one output sample, or returns false if more input is needed.
    bool Pop(float* y)
    {
        const size_t i = size_t(pos_);
        if (i + width_ >= buf_.size()) {
            return false;
        }
        const double pf = (pos_ - double(i)) * kPhases;
        const size_t p = size_t(pf);
        const float a = float(pf - double(p));
        const int taps = 2 * width_;
        const float* k0 = &table_[p * taps];
        const float* k1 = k0 + taps;
        const float* x = &buf_[i - (width_ - 1)];

        float acc = 0.0f;
        for (int j = 0; j < taps; ++j) {
            acc += x[j] * (k0[j] + a * (k1[j] - k0[j]));
        }
        *y = acc;

        pos_ += step_;
        Compact();
        return true;
    }

    // Input samples queued ahead of the read position.
    size_t BufferedInput() const
    {
        const double ahead = double(buf_.size()) - pos_ - width_;
        return ahead > 0 ? size_t(ahead) : 0;
    }

    // Output samples that can be produced right now.
    size_t Available() const { return size_t(double(BufferedInput()) / step_); }

private:
    static constexpr int kPhases = 512;
    static constexpr size_t kCapacity = 1 << 17;

    static double Sinc(double x)
    {
        return std::fabs(x) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
    }

    static double BesselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 32; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    static double Kaiser(double u)
    {
        constexpr double kBeta = 8.0;
        if (std::fabs(u) >= 1.0) {
            return 0.0;
        }
        return BesselI0(kBeta * std::sqrt(1.0 - u * u)) / BesselI0(kBeta);
    }

    void Compact()
    {
        const size_t first = size_t(pos_) - (width_ - 1);
        if (first > 4096) {
            buf_.erase(buf_.begin(), buf_.begin() + first);
            pos_ -= double(first);
        }
    }

    double step_;
    int width_ = 0;
    double pos_ = 0.0;
    std::vector<float> table_;
    std::vector<float> buf_;
};

} // namespace trusdx
