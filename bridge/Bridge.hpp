#pragma once

#include "AudioLink.hpp"
#include "Ports.hpp"
#include "Protocol.hpp"
#include "SpscRing.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace trusdx {

struct Options {
    std::string port;                       // empty: auto-detect
    std::string catLink = "/tmp/trusdx-cat"; // where the CAT client connects
    bool speaker = false;                   // keep the radio's speaker on (UA1)
    double rxRate = kRadioRxRateNominal;
    float txGain = 1.0f;
    int txTimeoutSec = 180;
    bool verbose = false;
};

// Snapshot for UI; safe to take from any thread.
struct BridgeStatus {
    bool radioConnected = false;
    bool audioConnected = false;
    bool transmitting = false;
    uint64_t frequencyHz = 0; // 0 until the radio has reported one
    std::string radioProblem; // connected but not usable, e.g. firmware without audio streaming
    std::string error;        // fatal startup error, if any
};

// Owns the radio's serial port and everything attached to it:
//   serial reader  - splits radio bytes into CAT replies and receive audio
//   serial writer  - sends CAT commands and, while transmitting, capture-paced audio
//   pty loop       - takes CAT commands from the client (WSJT-X/Hamlib)
//   audio link     - moves audio to and from the truSDX Core Audio device
class Bridge {
public:
    explicit Bridge(const Options& options);
    ~Bridge();

    // Runs until `stop` becomes true, reconnecting radio and audio as needed.
    int Run(const std::atomic<bool>& stop);

    BridgeStatus Status();
    void SetVerbose(bool enabled);
    // Switches the radio's streaming mode (UA1/UA2) without reconnecting.
    void SetSpeaker(bool on);

private:
    using Clock = std::chrono::steady_clock;

    void PtyLoop();
    void HandleClientCommand(const std::string& cmd);

    bool OpenRadio();
    void CloseRadio();
    void ReaderLoop();
    void WriterLoop();
    void InitRadio();
    void HandleRadioFrame(const std::string& frame);
    bool SendToRadio(const std::string& data);
    void Enqueue(const std::string& cmd);
    std::string CachedReply(const std::string& name);
    bool SendCatCommand(const std::string& cmd, bool transmitting);
    const char* AudioModeCommand() const { return speaker_ ? "UA1;" : "UA2;"; }
    void SendAudioMode(const std::string& prefix = "");
    bool WaitForAudio(std::chrono::milliseconds timeout);
    void LogStats();

    const Options options_;
    std::atomic<bool> verbose_{false};
    std::atomic<bool> speaker_{false};

    SpscRing<uint8_t> rxRaw_{1 << 15};
    SpscRing<float> txIn_{1 << 17};
    std::atomic<bool> txWanted_{false}; // PTT requested by the client
    std::atomic<bool> txActive_{false}; // radio is keyed and streaming our audio

    AudioLink audio_;
    CatPty pty_;
    std::thread ptyThread_;
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> audioOk_{false};
    std::atomic<bool> radioOk_{false};
    std::mutex errorMu_;
    std::string error_;

    // Per radio connection.
    std::atomic<int> serialFd_{-1}; // the pty thread checks it in Enqueue
    std::string serialPath_;
    std::thread reader_;
    std::thread writer_;
    std::atomic<bool> sessionStop_{false};
    std::atomic<bool> radioDead_{false};
    std::atomic<bool> forwardToClient_{false};
    RadioStreamParser parser_;

    std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::deque<std::string> queue_;

    std::mutex cacheMu_;
    std::map<std::string, std::string> cache_; // last reply per command name

    // Is the firmware actually streaming audio? (UA needs truSDX firmware 2.00t+)
    std::atomic<bool> audioRejected_{false}; // "?;" right after a UA command
    std::atomic<int64_t> lastAudioMs_{0};
    std::atomic<int64_t> lastUaSentMs_{0};
    std::atomic<int64_t> initDoneMs_{0};
    std::atomic<bool> recoveringAudio_{false}; // hide internal reset acknowledgements/errors
    std::atomic<bool> audioModeChanged_{false}; // speaker toggled; next reset applies it

    std::atomic<uint64_t> rxBytes_{0};
    std::atomic<uint64_t> txBytes_{0};

    // Run thread only.
    uint64_t statsLastRx_ = 0;  // rxBytes_ at the previous stats line
    std::string lastOpenError_; // last "cannot open" error logged, to avoid repeats
};

} // namespace trusdx
