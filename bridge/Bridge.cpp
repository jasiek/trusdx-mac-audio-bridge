#include "Bridge.hpp"

#include "Config.hpp"
#include "Log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace trusdx {

namespace {

using namespace std::chrono_literals;

constexpr size_t kTxBlock = 512; // reference bridge default block_size at 11520 Hz

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

Bridge::Bridge(const Options& options)
    : options_(options)
    , verbose_(options.verbose)
    , audio_(rxRaw_, txIn_, txWanted_, options.rxRate)
{
    pty_.SetVerbose(options.verbose);
}

void Bridge::SetVerbose(bool enabled)
{
    pty_.SetVerbose(enabled);
    if (verbose_.exchange(enabled) != enabled) {
        Log("verbose logging: %s", enabled ? "on" : "off");
    }
}

Bridge::~Bridge()
{
    CloseRadio();
    audio_.Stop();
    shutdown_ = true;
    if (ptyThread_.joinable()) {
        ptyThread_.join();
    }
}

int Bridge::Run(const std::atomic<bool>& stop)
{
    std::string error;
    if (!pty_.Open(options_.catLink, &error)) {
        Log("cat: %s", error.c_str());
        std::lock_guard<std::mutex> lock(errorMu_);
        error_ = error;
        return 1;
    }
    Log("cat: point your CAT client (Kenwood TS-480, 115200) at %s", options_.catLink.c_str());
    ptyThread_ = std::thread(&Bridge::PtyLoop, this);

    auto nextRadioTry = Clock::now();
    auto nextAudioTry = Clock::now();
    auto nextStats = Clock::now() + 10s;
    bool audioMissingLogged = false;
    bool radioMissingLogged = false;

    while (!stop) {
        const auto now = Clock::now();

        if (serialFd_ >= 0 && radioDead_) {
            Log("radio: lost %s", serialPath_.c_str());
            CloseRadio();
            nextRadioTry = now + 1s;
        }
        if (serialFd_ < 0 && now >= nextRadioTry) {
            if (OpenRadio()) {
                radioMissingLogged = false;
            } else {
                if (!radioMissingLogged) {
                    Log("radio: waiting for the truSDX USB serial port...");
                    radioMissingLogged = true;
                }
                nextRadioTry = now + 2s;
            }
        }

        if (!audio_.Healthy() && now >= nextAudioTry) {
            audio_.Stop();
            if (audio_.Start()) {
                audioMissingLogged = false;
            } else {
                if (!audioMissingLogged) {
                    Log("audio: truSDX device not found - is truSDX.driver installed?");
                    audioMissingLogged = true;
                }
                nextAudioTry = now + 3s;
            }
        }

        audioOk_ = audio_.Healthy();
        radioOk_ = serialFd_ >= 0 && forwardToClient_;

        if (verbose_ && now >= nextStats) {
            LogStats();
            nextStats = now + 10s;
        }
        std::this_thread::sleep_for(200ms);
    }

    Log("shutting down");
    return 0;
}

BridgeStatus Bridge::Status()
{
    BridgeStatus status;
    status.radioConnected = radioOk_;
    status.audioConnected = audioOk_;
    status.transmitting = txActive_;
    const std::string fa = CachedReply("FA"); // "FA00014074000;"
    if (fa.size() == 14) {
        status.frequencyHz = std::strtoull(fa.c_str() + 2, nullptr, 10);
    }
    if (radioOk_ && initDoneMs_ > 0) {
        const bool streaming = txActive_ || NowMs() - lastAudioMs_ < 2000;
        if (!streaming && audioRejected_) {
            status.radioProblem = "firmware has no USB audio (needs 2.00t+)";
        } else if (!streaming) {
            status.radioProblem = "no audio stream from radio";
        }
    }
    std::lock_guard<std::mutex> lock(errorMu_);
    status.error = error_;
    return status;
}

void Bridge::LogStats()
{
    static uint64_t lastRx = 0;
    const uint64_t rx = rxBytes_.load();
    Log("stats: radio %s, serial in %.0f B/s, rx rate %.1f Hz, underruns %llu, tx %s",
        serialFd_ >= 0 ? "connected" : "absent", (rx - lastRx) / 10.0, audio_.EstimatedRxRate(),
        (unsigned long long)audio_.Underruns(), txActive_ ? "ON" : "off");
    lastRx = rx;
}

// ---- CAT client side ------------------------------------------------------

void Bridge::PtyLoop()
{
    CatSplitter splitter;
    char buf[512];
    while (!shutdown_) {
        pollfd p = {pty_.Fd(), POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) {
            continue;
        }
        const ssize_t n = pty_.Read(buf, sizeof buf);
        if (n > 0) {
            splitter.Feed(buf, size_t(n), [this](const std::string& cmd) { HandleClientCommand(cmd); });
        }
    }
}

void Bridge::HandleClientCommand(const std::string& cmd)
{
    // The reference answers ID locally to meet Hamlib's RX;ID; turnaround.
    // Every other command, including PTT, stays ordered in the writer queue.
    if (ClassifyClientCommand(cmd) == ClientAction::LocalId) {
        pty_.Write("ID020;");
        return;
    }
    Enqueue(cmd);
}

// ---- Radio side -----------------------------------------------------------

bool Bridge::OpenRadio()
{
    const std::string path = options_.port.empty() ? FindRadioPort() : options_.port;
    if (path.empty()) {
        return false;
    }
    std::string error;
    const int fd = OpenSerial(path, &error);
    if (fd < 0) {
        static std::string lastError;
        if (error != lastError) {
            Log("radio: cannot open %s: %s", path.c_str(), error.c_str());
            lastError = error;
        }
        return false;
    }

    serialFd_ = fd;
    serialPath_ = path;
    sessionStop_ = false;
    radioDead_ = false;
    forwardToClient_ = false;
    txWanted_ = false;
    txActive_ = false;
    audioRejected_ = false;
    lastAudioMs_ = 0;
    initDoneMs_ = 0;
    recoveringAudio_ = false;
    parser_.Reset();
    {
        std::lock_guard<std::mutex> lock(cacheMu_);
        cache_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queueMu_);
        queue_.clear();
    }

    Log("radio: opened %s", path.c_str());
    reader_ = std::thread(&Bridge::ReaderLoop, this);
    writer_ = std::thread(&Bridge::WriterLoop, this);
    return true;
}

void Bridge::CloseRadio()
{
    if (serialFd_ < 0) {
        return;
    }
    sessionStop_ = true;
    queueCv_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (!radioDead_) {
        // Unkey, stop streaming and give the speaker back.
        SendCatCommand("RX;", txActive_);
        SendCatCommand("UA0;", false);
        // Give these short shutdown commands time to leave USB before close.
        // Do not use the driver's potentially multi-second tcdrain here.
        std::this_thread::sleep_for(20ms);
    }
    close(serialFd_);
    serialFd_ = -1;
    txActive_ = false;
    forwardToClient_ = false;
}

bool Bridge::SendToRadio(const std::string& data)
{
    if (!WriteAll(serialFd_, data.data(), data.size())) {
        radioDead_ = true;
        return false;
    }
    return true;
}

void Bridge::Enqueue(const std::string& cmd)
{
    if (serialFd_ < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queueMu_);
        queue_.push_back(cmd);
    }
    queueCv_.notify_all();
}

std::string Bridge::CachedReply(const std::string& name)
{
    std::lock_guard<std::mutex> lock(cacheMu_);
    auto it = cache_.find(name);
    return it == cache_.end() ? std::string() : it->second;
}

bool Bridge::SendCatCommand(const std::string& cmd, bool transmitting)
{
    if (transmitting && ClassifyClientCommand(cmd) == ClientAction::PttOff) {
        // A final 512-byte audio block takes 44.4 ms at 115200 8N1.
        // Stop capture and allow it to finish, then separate the stream
        // terminator from RX so firmware can leave its audio-input state.
        // tcflush followed by a combined ;RX; was unreliable on 2.00x;
        // tcdrain instead blocked the CH340/macOS path for ~3.3 seconds.
        txWanted_ = false;
        std::this_thread::sleep_for(60ms);
        if (!SendToRadio(";")) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
        return SendToRadio(cmd);
    }
    // The reader delivers replies independently. Draining here can stall the
    // writer for seconds on macOS even after the radio has answered, leaving
    // the client's next command queued until its CAT timeout expires.
    return SendToRadio(cmd);
}

void Bridge::ReaderLoop()
{
    uint8_t buf[4096];
    while (!sessionStop_) {
        pollfd p = {serialFd_, POLLIN, 0};
        const int r = poll(&p, 1, 200);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 || (p.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            radioDead_ = true;
            return;
        }
        if (r == 0) {
            continue;
        }
        const ssize_t n = read(serialFd_, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            radioDead_ = true;
            return;
        }
        rxBytes_ += uint64_t(n);
        parser_.Feed(
            buf, size_t(n),
            [this](const uint8_t* samples, size_t count) {
                lastAudioMs_.store(NowMs(), std::memory_order_relaxed);
                audioRejected_.store(false, std::memory_order_relaxed);
                if (!txActive_) {
                    rxRaw_.Push(samples, count);
                }
            },
            [this](const std::string& frame) { HandleRadioFrame(frame); });
    }
}

void Bridge::HandleRadioFrame(const std::string& frame)
{
    if (!IsPlausibleCatFrame(frame)) {
        return;
    }
    const std::string name = CommandName(frame);
    if (frame.size() > 3) {
        std::lock_guard<std::mutex> lock(cacheMu_);
        cache_[name] = frame;
    }
    if (verbose_) {
        Log("radio CAT <- radio %s", frame.c_str());
    }
    if (frame == "?;" && NowMs() - lastUaSentMs_ < 500) {
        audioRejected_ = true;
    }
    if (!forwardToClient_ || (recoveringAudio_ && (name == "UA" || frame == "?;"))) {
        return;
    }
    pty_.Write(frame);
}

void Bridge::InitRadio()
{
    // The reference allows three seconds for boot, then sets USB and audio mode.
    // Keep the native bridge's startup unkey in front of that same initialization.
    const auto bootDeadline = Clock::now() + 3s;
    while (Clock::now() < bootDeadline && !sessionStop_ && !radioDead_) {
        std::this_thread::sleep_for(50ms);
    }
    if (sessionStop_ || radioDead_) {
        return;
    }
    SendAudioMode(";RX;MD2;");
    if (WaitForAudio(1500ms)) {
        Log("radio: audio streaming active");
    } else if (audioRejected_) {
        Log("radio: firmware rejected %s - CAT works, but audio needs truSDX firmware 2.00t+",
            AudioModeCommand());
    } else {
        Log("radio: no audio stream after %s", AudioModeCommand());
    }
    // Read initial radio status before exposing the CAT endpoint to clients.
    // These startup replies must not leak into a client's pending transaction.
    for (const char* name : {"FA", "MD", "IF"}) {
        {
            std::lock_guard<std::mutex> lock(cacheMu_);
            cache_.erase(name);
        }
        if (sessionStop_ || radioDead_ || !SendToRadio(std::string(name) + ";")) {
            return;
        }
        // The firmware can reject IF when probes are sent back to back.
        // Wait for each response before issuing the next startup query.
        const auto cacheDeadline = Clock::now() + 500ms;
        while (!sessionStop_ && !radioDead_ && Clock::now() < cacheDeadline
            && CachedReply(name).empty()) {
            std::this_thread::sleep_for(5ms);
        }
        if (CachedReply(name).empty()) {
            Log("radio: no confirmed initial %s state", name);
        }
    }
    initDoneMs_ = NowMs();
    forwardToClient_ = true;
}

void Bridge::SendAudioMode(const std::string& prefix)
{
    lastUaSentMs_ = NowMs();
    SendToRadio(prefix + AudioModeCommand());
}

bool Bridge::WaitForAudio(std::chrono::milliseconds timeout)
{
    const int64_t since = NowMs();
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline && !sessionStop_ && !radioDead_) {
        if (lastAudioMs_ >= since) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return lastAudioMs_ >= since;
}

void Bridge::WriterLoop()
{
    InitRadio();

    Resampler txResampler(kDeviceSampleRate / kRadioTxRate);
    bool tx = false;
    bool discardFirstBlock = false;
    Clock::time_point txStart;
    size_t txAudio = 0;
    std::vector<float> scratch(4096);
    std::vector<uint8_t> txBlock(kTxBlock);
    int64_t rxSinceMs = NowMs();
    int64_t lastRecoveryMs = NowMs() - 5000;

    enum class Recovery { None, DisableAudio, EnableAudio, ReassertRx, WaitAudio };
    Recovery recovery = Recovery::None;
    int64_t recoveryDeadlineMs = 0;
    int64_t recoveryAudioSinceMs = 0;
    auto beginRecovery = [&](bool alreadyUnkeyed) {
        recoveringAudio_ = true;
        lastRecoveryMs = NowMs();
        if (!alreadyUnkeyed) SendToRadio("RX;");
        recovery = Recovery::DisableAudio;
        recoveryDeadlineMs = NowMs() + 50;
    };
    auto canHandleDuringRecovery = [](const std::string& cmd) {
        return cmd == "FA;" || cmd == "MD;" || cmd == "IF;" || cmd == "RX;";
    };
    auto replyFromCache = [&](const std::string& cmd) {
        std::string reply;
        {
            std::lock_guard<std::mutex> lock(cacheMu_);
            auto get = [&](const char* name) {
                auto it = cache_.find(name);
                return it == cache_.end() ? std::string() : it->second;
            };
            reply = CachedStatusReply(cmd, get("FA"), get("MD"), get("IF"), tx);
        }
        pty_.Write(reply);
    };

    auto sendCommand = [&](const std::string& cmd) {
        const auto action = ClassifyClientCommand(cmd);
        if (recovery != Recovery::None) {
            // RX is already satisfied; do not let redundant unkeys delay polls.
            if (cmd != "RX;") replyFromCache(cmd);
            return;
        }
        if (tx && action != ClientAction::PttOff) {
            // O; makes Hamlib fail IF polling. Repeated TX must not reset capture.
            if (action != ClientAction::PttOn) replyFromCache(cmd);
            return;
        }
        const bool endingTx = tx && action == ClientAction::PttOff;
        // Hide any internal unkey error before its write can reach the reader.
        if (endingTx) recoveringAudio_ = true;
        if (verbose_) {
            Log("radio CAT -> radio %s", cmd.c_str());
        }
        if (!SendCatCommand(cmd, tx)) {
            return;
        }
        switch (action) {
        case ClientAction::PttOn:
            // Reference: restart capture and read/discard one 512-frame block.
            txIn_.Clear();
            txResampler.Reset();
            discardFirstBlock = true;
            audio_.TxPeak();
            txAudio = 0;
            txStart = Clock::now();
            tx = true;
            txWanted_ = true;
            txActive_ = true;
            Log("ptt: TX");
            break;
        case ClientAction::PttOff: {
            const double keyed = tx
                ? std::chrono::duration<double>(Clock::now() - txStart).count() : 0.0;
            tx = false;
            txWanted_ = false;
            txActive_ = false;
            txIn_.Clear();
            txResampler.Reset();
            rxSinceMs = NowMs();
            Log("ptt: RX (keyed %.1f s: sent %.1f s audio, link %.0f B/s of %.0f, app peak %.2f)",
                keyed, txAudio / kRadioTxRate,
                keyed > 0 ? txAudio / keyed : 0.0, kRadioTxRate, audio_.TxPeak());
            if (endingTx) {
                Log("radio: TX ended - resetting serial audio immediately");
                beginRecovery(true);
            }
            break;
        }
        case ClientAction::LocalId:
        case ClientAction::Forward:
            break;
        }
    };

    while (!sessionStop_ && !radioDead_) {
        // Like handle_cat(), process one complete command before each audio block.
        std::string cmd;
        {
            std::lock_guard<std::mutex> lock(queueMu_);
            // Preserve command order: setters and another TX wait for the reset.
            if (!queue_.empty() && (recovery == Recovery::None
                    || canHandleDuringRecovery(queue_.front()))) {
                cmd = queue_.front();
                queue_.pop_front();
            }
        }
        if (!cmd.empty()) {
            sendCommand(cmd);
        }
        if (radioDead_) {
            break;
        }

        if (!tx) {
            const int64_t nowMs = NowMs();
            if (recovery == Recovery::None && lastAudioMs_ > 0
                && nowMs - lastAudioMs_ > 2000
                && nowMs - rxSinceMs > 1000 && nowMs - lastRecoveryMs > 5000) {
                Log("radio: no receive audio for %.0f s - resetting serial audio",
                    (nowMs - lastAudioMs_) / 1000.0);
                beginRecovery(false);
            }
            // Advance reset deadlines without sleeping through CAT status polls.
            // All serial writes stay here, so a queued TX cannot interrupt reset.
            if (recovery != Recovery::None && nowMs >= recoveryDeadlineMs) {
                switch (recovery) {
                case Recovery::DisableAudio:
                    SendToRadio("UA0;");
                    recovery = Recovery::EnableAudio;
                    recoveryDeadlineMs = NowMs() + 200;
                    break;
                case Recovery::EnableAudio:
                    SendAudioMode();
                    recovery = Recovery::ReassertRx;
                    recoveryDeadlineMs = NowMs() + 50;
                    break;
                case Recovery::ReassertRx:
                    recoveryAudioSinceMs = NowMs();
                    SendToRadio("RX;");
                    recovery = Recovery::WaitAudio;
                    recoveryDeadlineMs = NowMs() + 500;
                    break;
                case Recovery::WaitAudio:
                case Recovery::None:
                    break;
                }
            }
            if (recovery == Recovery::WaitAudio) {
                const bool resumed = lastAudioMs_ >= recoveryAudioSinceMs;
                if (resumed || NowMs() >= recoveryDeadlineMs) {
                    Log(resumed ? "radio: receive audio recovered"
                                : "radio: serial audio reset did not restore samples");
                    recovery = Recovery::None;
                    recoveringAudio_ = false;
                }
            }
            std::unique_lock<std::mutex> lock(queueMu_);
            queueCv_.wait_for(lock, 1ms, [&] {
                return sessionStop_ || (!queue_.empty() && (recovery == Recovery::None
                    || canHandleDuringRecovery(queue_.front())));
            });
            continue;
        }

        if (Clock::now() - txStart > std::chrono::seconds(options_.txTimeoutSec)) {
            Log("ptt: transmit timeout (%d s) - forcing RX", options_.txTimeoutSec);
            sendCommand("RX;");
            continue;
        }

        size_t n;
        while ((n = txIn_.Pop(scratch.data(), scratch.size())) > 0) {
            for (size_t i = 0; i < n; ++i) {
                txResampler.Push(scratch[i] * options_.txGain);
            }
        }
        if (txResampler.Available() < kTxBlock) {
            std::unique_lock<std::mutex> lock(queueMu_);
            queueCv_.wait_for(lock, 1ms, [&] { return !queue_.empty() || sessionStop_; });
            continue;
        }
        for (size_t i = 0; i < kTxBlock; ++i) {
            float y;
            txResampler.Pop(&y);
            txBlock[i] = FloatToU8(y);
        }
        if (discardFirstBlock) {
            discardFirstBlock = false;
            continue;
        }
        // Capture at 11520 Hz paces the reference. Here the 48 kHz device feeds
        // a resampler at that rate; write each available block, without filler.
        if (!WriteAll(serialFd_, txBlock.data(), txBlock.size())) {
            radioDead_ = true;
            break;
        }
        txAudio += txBlock.size();
        txBytes_ += txBlock.size();
    }

    if (tx && !radioDead_) {
        SendCatCommand("RX;", true);
    }
    txWanted_ = false;
    txActive_ = false;
    recoveringAudio_ = false;
}

} // namespace trusdx
