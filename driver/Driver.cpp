// Core Audio server plug-in exposing the "truSDX" audio device.
//
// Two devices are published:
//   truSDX          - visible; what WSJT-X and friends select.
//   truSDX (bridge) - hidden; opened only by the trusdx-bridge daemon.
// Output written to one device appears as input on the other, so the daemon
// plays radio audio into the bridge device and apps record it from truSDX, and
// vice versa for transmit audio. All of this stays inside coreaudiod; the
// daemon is an ordinary Core Audio client and needs no private IPC channel.

#include "Config.hpp"
#include "TimelineRing.hpp"

#include <aspl/Driver.hpp>

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>

#include <cmath>

namespace {

using namespace trusdx;

// Both devices report zero timestamps from this one anchor, so a given sample
// time names the same instant on either device and the rings can be indexed by it.
struct SharedClock {
    UInt64 anchor = mach_absolute_time();
    Float64 ticksPerFrame = [] {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return Float64(tb.denom) / tb.numer * 1e9 / kDeviceSampleRate;
    }();
};

const SharedClock& Clock()
{
    static const SharedClock clock;
    return clock;
}

TimelineRing gRadioToApp; // receive audio: bridge output -> truSDX input
TimelineRing gAppToRadio; // transmit audio: truSDX output -> bridge input

class LoopDevice : public aspl::Device
{
public:
    LoopDevice(std::shared_ptr<const aspl::Context> context,
        const aspl::DeviceParameters& params,
        bool hidden)
        : aspl::Device(std::move(context), params)
        , hidden_(hidden)
    {
    }

    bool GetIsHidden() const override
    {
        return hidden_;
    }

protected:
    OSStatus GetZeroTimeStampImpl(UInt32 clientID,
        Float64* outSampleTime,
        UInt64* outHostTime,
        UInt64* outSeed) override
    {
        const SharedClock& clock = Clock();
        const Float64 period = GetZeroTimeStampPeriod();
        const Float64 ticksPerPeriod = clock.ticksPerFrame * period;
        const UInt64 n = UInt64(Float64(mach_absolute_time() - clock.anchor) / ticksPerPeriod);

        *outSampleTime = Float64(n) * period;
        *outHostTime = clock.anchor + UInt64(Float64(n) * ticksPerPeriod);
        *outSeed = 1;
        return kAudioHardwareNoError;
    }

private:
    const bool hidden_;
};

class LoopHandler : public aspl::IORequestHandler
{
public:
    LoopHandler(TimelineRing& readFrom, TimelineRing& writeTo)
        : readFrom_(readFrom)
        , writeTo_(writeTo)
    {
    }

    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        void* bytes,
        UInt32 bytesCount) override
    {
        readFrom_.Read(std::llround(timestamp), static_cast<float*>(bytes),
            bytesCount / sizeof(float));
    }

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        const void* buff,
        UInt32 buffBytesSize) override
    {
        writeTo_.Write(std::llround(timestamp), static_cast<const float*>(buff),
            buffBytesSize / sizeof(float));
    }

private:
    TimelineRing& readFrom_;
    TimelineRing& writeTo_;
};

aspl::StreamParameters MonoFloatStream(aspl::Direction dir)
{
    aspl::StreamParameters params;
    params.Direction = dir;
    params.Format = {
        .mSampleRate = kDeviceSampleRate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags =
            kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = sizeof(float),
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 32,
    };
    return params;
}

std::shared_ptr<LoopDevice> MakeDevice(const std::shared_ptr<aspl::Context>& context,
    const char* name,
    const char* uid,
    bool hidden,
    bool withControls,
    TimelineRing& readFrom,
    TimelineRing& writeTo)
{
    aspl::DeviceParameters params;
    params.Name = name;
    params.Manufacturer = "trusdx-mac-audio-bridge";
    params.DeviceUID = uid;
    params.ModelUID = "trusdx-audio";
    params.SampleRate = UInt32(kDeviceSampleRate);
    params.ChannelCount = 1;
    params.EnableMixing = true;
    // Never let macOS route alerts or music to the transmitter.
    params.CanBeDefault = false;
    params.CanBeDefaultForSystemSounds = false;

    auto device = std::make_shared<LoopDevice>(context, params, hidden);
    for (auto dir : {aspl::Direction::Input, aspl::Direction::Output}) {
        if (withControls) {
            device->AddStreamWithControlsAsync(MonoFloatStream(dir));
        } else {
            device->AddStreamAsync(MonoFloatStream(dir));
        }
    }
    device->SetIOHandler(std::make_shared<LoopHandler>(readFrom, writeTo));
    return device;
}

std::shared_ptr<aspl::Driver> CreateDriver()
{
    auto context = std::make_shared<aspl::Context>();
    auto plugin = std::make_shared<aspl::Plugin>(context);

    plugin->AddDevice(MakeDevice(context, "truSDX", kMainDeviceUID, false, true,
        gRadioToApp, gAppToRadio));
    plugin->AddDevice(MakeDevice(context, "truSDX (bridge)", kBridgeDeviceUID, true, false,
        gAppToRadio, gRadioToApp));

    return std::make_shared<aspl::Driver>(context, plugin);
}

} // namespace

extern "C" void* TruSDXDriverEntryPoint(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }
    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    return driver->GetReference();
}
