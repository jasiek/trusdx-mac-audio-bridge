#pragma once

// Values shared by the Core Audio driver and the bridge daemon.
namespace trusdx {

constexpr double kDeviceSampleRate = 48000.0;

// The device apps (WSJT-X, fldigi, ...) use.
constexpr const char* kMainDeviceUID = "trusdx-audio:main";
// Hidden twin the bridge daemon uses; its I/O is cross-wired with the main device.
constexpr const char* kBridgeDeviceUID = "trusdx-audio:bridge";

} // namespace trusdx
