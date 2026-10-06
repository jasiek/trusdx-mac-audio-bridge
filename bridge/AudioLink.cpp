#include "AudioLink.hpp"

#include "Config.hpp"
#include "Log.hpp"
#include "Protocol.hpp"

#include <algorithm>
#include <cstring>
#include <time.h>

namespace trusdx {

namespace {

constexpr UInt32 kBufferFrames = 256;
constexpr double kRxLatencySec = 0.08; // receive audio held back to absorb USB bursts

uint64_t NowNs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

AudioObjectID FindDevice(const char* uid)
{
    const AudioObjectPropertyAddress addr = {kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef uidRef = CFStringCreateWithCString(nullptr, uid, kCFStringEncodingUTF8);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const OSStatus err = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &addr, sizeof(uidRef), &uidRef, &size, &device);
    CFRelease(uidRef);
    return err == noErr ? device : kAudioObjectUnknown;
}

} // namespace

AudioLink::AudioLink(SpscRing<uint8_t>& rxRaw,
    SpscRing<float>& txIn,
    const std::atomic<bool>& txWanted,
    double rxRate)
    : rxRaw_(rxRaw)
    , txIn_(txIn)
    , txWanted_(txWanted)
    , rxRate_(rxRate)
    , nominalStep_(rxRate / kDeviceSampleRate)
    , rxResampler_(rxRate / kDeviceSampleRate)
{
}

AudioLink::~AudioLink()
{
    Stop();
}

bool AudioLink::Start()
{
    device_ = FindDevice(kBridgeDeviceUID);
    if (device_ == kAudioObjectUnknown) {
        return false;
    }

    UInt32 frames = kBufferFrames;
    const AudioObjectPropertyAddress bufAddr = {kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(device_, &bufAddr, 0, nullptr, sizeof(frames), &frames);

    rxRaw_.Clear();
    rxResampler_.Reset();
    prefilling_ = true;
    lastCallbackNs_ = NowNs();

    OSStatus err = AudioDeviceCreateIOProcID(device_, &AudioLink::IOProc, this, &procId_);
    if (err == noErr) {
        err = AudioDeviceStart(device_, procId_);
    }
    if (err != noErr) {
        Log("audio: cannot start truSDX bridge device (OSStatus %d)", int(err));
        Stop();
        return false;
    }
    Log("audio: connected to truSDX bridge device");
    return true;
}

void AudioLink::Stop()
{
    if (procId_) {
        AudioDeviceStop(device_, procId_);
        AudioDeviceDestroyIOProcID(device_, procId_);
        procId_ = nullptr;
    }
    device_ = kAudioObjectUnknown;
}

bool AudioLink::Healthy() const
{
    return procId_ && NowNs() - lastCallbackNs_.load() < 2'000'000'000ull;
}

double AudioLink::EstimatedRxRate() const
{
    return rxRate_ * (1.0 + integShared_.load());
}

OSStatus AudioLink::IOProc(AudioObjectID,
    const AudioTimeStamp*,
    const AudioBufferList* input,
    const AudioTimeStamp*,
    AudioBufferList* output,
    const AudioTimeStamp*,
    void* self)
{
    static_cast<AudioLink*>(self)->Process(input, output);
    return noErr;
}

// Process runs on the real-time I/O thread; txPeak_ must not take a lock.
static_assert(std::atomic<float>::is_always_lock_free, "txPeak_ must be lock-free");

void AudioLink::Process(const AudioBufferList* input, AudioBufferList* output)
{
    lastCallbackNs_.store(NowNs(), std::memory_order_relaxed);

    // Transmit: app audio from the truSDX output, via the bridge input.
    if (input && input->mNumberBuffers > 0 && input->mBuffers[0].mData) {
        const float* in = static_cast<const float*>(input->mBuffers[0].mData);
        const size_t frames = input->mBuffers[0].mDataByteSize / sizeof(float);
        if (txWanted_.load(std::memory_order_relaxed)) {
            txIn_.Push(in, frames);
            float peak = 0.0f;
            for (size_t i = 0; i < frames; ++i) {
                peak = std::max(peak, std::fabs(in[i]));
            }
            // Atomic fetch-max, so a concurrent TxPeak() reset isn't overwritten.
            float cur = txPeak_.load(std::memory_order_relaxed);
            while (peak > cur && !txPeak_.compare_exchange_weak(cur, peak, std::memory_order_relaxed)) {
            }
        }
    }

    // Receive: radio audio to the bridge output, which apps read from truSDX.
    if (output && output->mNumberBuffers > 0 && output->mBuffers[0].mData) {
        RenderRx(static_cast<float*>(output->mBuffers[0].mData),
            output->mBuffers[0].mDataByteSize / sizeof(float));
    }
}

void AudioLink::RenderRx(float* out, size_t frames)
{
    uint8_t raw[512];
    size_t n;
    while ((n = rxRaw_.Pop(raw, sizeof raw)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            // One-pole DC blocker: the radio's samples sit a few counts above 128.
            const float x = U8ToFloat(raw[i]);
            dc_ += 0.0008f * (x - dc_);
            rxResampler_.Push(x - dc_);
        }
    }

    const double target = rxRate_ * kRxLatencySec;
    const double fill = double(rxResampler_.BufferedInput());

    if (fill > 4 * target) {
        // Backlog from before we started (or a stall); drop it rather than lag.
        rxResampler_.Reset();
        prefilling_ = true;
    }
    if (prefilling_) {
        if (fill < target) {
            std::memset(out, 0, frames * sizeof(float));
            return;
        }
        prefilling_ = false;
        fillAvg_ = fill;
    }

    // Steer the ratio so the backlog stays at `target`, tracking the radio's clock.
    fillAvg_ += 0.02 * (fill - fillAvg_);
    const double err = (fillAvg_ - target) / target;
    integ_ = std::clamp(integ_ + 2e-5 * err, -0.01, 0.01);
    const double corr = std::clamp(0.004 * err + integ_, -0.02, 0.02);
    rxResampler_.SetStep(nominalStep_ * (1.0 + corr));
    integShared_.store(integ_, std::memory_order_relaxed);

    for (size_t i = 0; i < frames; ++i) {
        if (!rxResampler_.Pop(&out[i])) {
            std::memset(out + i, 0, (frames - i) * sizeof(float));
            prefilling_ = true;
            underruns_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

} // namespace trusdx
