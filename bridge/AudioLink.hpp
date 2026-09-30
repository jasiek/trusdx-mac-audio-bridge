#pragma once

#include "Resampler.hpp"
#include "SpscRing.hpp"

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cstdint>

namespace trusdx {

// Runs an I/O callback on the hidden "truSDX (bridge)" device:
//  - receive: 8-bit radio samples from `rxRaw` are DC-blocked, resampled to
//    48 kHz with a ratio that tracks the radio's clock, and played out;
//  - transmit: while `txWanted` is set, 48 kHz input is pushed to `txIn`.
class AudioLink {
public:
    AudioLink(SpscRing<uint8_t>& rxRaw,
        SpscRing<float>& txIn,
        const std::atomic<bool>& txWanted,
        double rxRate);
    ~AudioLink();

    bool Start();
    void Stop();

    // False when not started or when the device stopped calling back.
    bool Healthy() const;

    // Radio receive rate as tracked by the drift loop.
    double EstimatedRxRate() const;
    uint64_t Underruns() const { return underruns_.load(); }
    float TxPeak() { return txPeak_.exchange(0.0f); }

private:
    static OSStatus IOProc(AudioObjectID device,
        const AudioTimeStamp* now,
        const AudioBufferList* input,
        const AudioTimeStamp* inputTime,
        AudioBufferList* output,
        const AudioTimeStamp* outputTime,
        void* self);
    void Process(const AudioBufferList* input, AudioBufferList* output);
    void RenderRx(float* out, size_t frames);

    SpscRing<uint8_t>& rxRaw_;
    SpscRing<float>& txIn_;
    const std::atomic<bool>& txWanted_;
    const double rxRate_;
    const double nominalStep_;

    AudioObjectID device_ = kAudioObjectUnknown;
    AudioDeviceIOProcID procId_ = nullptr;

    // Receive state, touched only on the I/O thread.
    Resampler rxResampler_;
    float dc_ = 0.0f;
    bool prefilling_ = true;
    double fillAvg_ = 0.0;
    double integ_ = 0.0;

    std::atomic<double> integShared_{0.0};
    std::atomic<uint64_t> lastCallbackNs_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<float> txPeak_{0.0f};
};

} // namespace trusdx
