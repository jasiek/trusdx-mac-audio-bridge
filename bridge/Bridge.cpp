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

constexpr size_t kTxPrefill = 1152;          // ~100 ms of 11520 Hz audio before streaming
constexpr double kTxLeadBytes = 256;         // how far ahead of real time we keep the link
constexpr auto kTxStartTimeout = 300ms;      // stream silence if the app is slow to start audio

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

Bridge::Bridge(const Options& options)
    : options_(options)
    , audio_(rxRaw_, txIn_, txWanted_, options.rxRate)
{
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

        if (options_.verbose && now >= nextStats) {
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
        if (!idAnswered_) {
            status.radioProblem = "not answering CAT at 115200 baud (needs firmware 2.00t+)";
        } else if (!streaming && audioRejected_) {
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
        const ssize_t n = read(pty_.Fd(), buf, sizeof buf);
        if (n > 0) {
            splitter.Feed(buf, size_t(n), [this](const std::string& cmd) { HandleClientCommand(cmd); });
        }
    }
}

void Bridge::HandleClientCommand(const std::string& cmd)
{
    if (options_.verbose) {
        Log("cat <- client %s", cmd.c_str());
    }
    switch (ClassifyClientCommand(cmd)) {
    case ClientAction::PttOn:
        txWanted_ = true;
        queueCv_.notify_all();
        return;
    case ClientAction::PttOff:
        txWanted_ = false;
        queueCv_.notify_all();
        return;
    case ClientAction::Drop:
        return;
    case ClientAction::Forward:
        break;
    }

    // While keyed the radio is busy with our audio stream; answer polls from the
    // last replies we saw instead of punching holes in the stream.
    if (txActive_ && IsQuery(cmd)) {
        const std::string name = CommandName(cmd);
        std::string reply = CachedReply(name);
        if (!reply.empty()) {
            pty_.Write(name == "IF" ? WithTxFlag(reply, true) : reply);
            return;
        }
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
    idAnswered_ = false;
    audioRejected_ = false;
    lastAudioMs_ = 0;
    initDoneMs_ = 0;
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
        SendToRadio(";RX;UA0;");
        tcdrain(serialFd_);
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

void Bridge::SuppressErrorsFor(std::chrono::milliseconds d)
{
    suppressErrorsUntil_ = NowMs() + d.count();
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
    if (options_.verbose) {
        Log("cat -> radio reply %s", frame.c_str());
    }
    if (frame == "?;" && NowMs() - lastUaSentMs_ < 500) {
        audioRejected_ = true;
    }
    if (!forwardToClient_) {
        return;
    }
    // Echoes of the bridge's own streaming/PTT commands mean nothing to the client.
    if (name == "UA" || name == "TX" || name == "RX") {
        return;
    }
    if ((frame == "?;" || frame == "E;") && NowMs() < suppressErrorsUntil_) {
        return;
    }
    pty_.Write(frame);
}

void Bridge::InitRadio()
{
    // ";" ends any audio stream a previous session left open, RX; unkeys.
    SendToRadio(";RX;");
    std::this_thread::sleep_for(100ms);

    bool answered = false;
    for (int i = 0; i < 12 && !sessionStop_ && !radioDead_; ++i) {
        SendToRadio("ID;FA;");
        std::this_thread::sleep_for(500ms);
        if (!CachedReply("ID").empty()) {
            answered = true;
            break;
        }
    }
    idAnswered_ = answered;
    if (answered) {
        Log("radio: answering CAT (%s), frequency %s", CachedReply("ID").c_str(),
            CachedReply("FA").c_str());
    } else {
        Log("radio: no reply to ID; at 115200 baud - truSDX firmware 2.00t or newer is needed");
    }

    SendAudioMode();
    if (WaitForAudio(1500ms)) {
        Log("radio: audio streaming active");
    } else if (audioRejected_) {
        Log("radio: firmware rejected %s - CAT works, but audio needs truSDX firmware 2.00t+",
            AudioModeCommand());
    } else {
        Log("radio: no audio stream after %s - will keep retrying", AudioModeCommand());
    }
    initDoneMs_ = NowMs();
    forwardToClient_ = true;
}

void Bridge::SendAudioMode(const std::string& prefix)
{
    lastUaSentMs_ = NowMs();
    SendToRadio(prefix + AudioModeCommand());
    SuppressErrorsFor(300ms);
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
    bool streaming = false;
    Clock::time_point txStart;
    Clock::time_point streamStart;
    double sent = 0;
    size_t txAudio = 0;  // samples of app audio sent this transmission
    size_t txFiller = 0; // silence sent because app audio wasn't there
    std::vector<float> scratch(4096);
    std::vector<uint8_t> txChunk(4096); // up to ~0.36 s of catch-up per write

    while (!sessionStop_ && !radioDead_) {
        const bool want = txWanted_;

        if (want && !tx) {
            txIn_.Clear();
            txResampler.Reset();
            audio_.TxPeak(); // reset
            txAudio = txFiller = 0;
            SendToRadio("TX0;US");
            tx = true;
            streaming = false;
            txStart = Clock::now();
            txActive_ = true;
            Log("ptt: TX");
        } else if (!want && tx) {
            SendToRadio(";RX;");
            tcdrain(serialFd_);
            std::this_thread::sleep_for(80ms);
            // Repeat RX; (harmless if already receiving) and restore streaming mode.
            SendAudioMode("RX;");
            tx = false;
            txActive_ = false;
            const double keyed = streaming
                ? std::chrono::duration<double>(Clock::now() - streamStart).count()
                : 0.0;
            Log("ptt: RX (keyed %.1f s: sent %.1f s audio + %.1f s filler, link %.0f B/s "
                "of %.0f, app peak %.2f)",
                keyed, txAudio / kRadioTxRate, txFiller / kRadioTxRate,
                keyed > 0 ? (txAudio + txFiller) / keyed : 0.0, kRadioTxRate, audio_.TxPeak());
        }

        std::deque<std::string> cmds;
        {
            std::lock_guard<std::mutex> lock(queueMu_);
            cmds.swap(queue_);
        }
        for (const std::string& cmd : cmds) {
            if (options_.verbose) {
                Log("cat -> radio %s", cmd.c_str());
            }
            // While streaming, ';' pauses the audio and "US" resumes it.
            SendToRadio(tx ? ";" + cmd + "US" : cmd);
        }

        if (!tx) {
            // Receive audio stopped (e.g. the radio was power-cycled while USB stayed up):
            // ask for the stream again, at most every few seconds.
            const int64_t nowMs = NowMs();
            if (nowMs - lastAudioMs_ > 2000 && nowMs - lastUaSentMs_ > 5000) {
                if (options_.verbose || lastAudioMs_ > 0) {
                    Log("radio: no receive audio for %.0f s - re-sending %s",
                        (nowMs - lastAudioMs_) / 1000.0, AudioModeCommand());
                }
                SendAudioMode();
            }

            std::unique_lock<std::mutex> lock(queueMu_);
            queueCv_.wait_for(lock, 20ms, [&] {
                return !queue_.empty() || txWanted_ != tx || sessionStop_;
            });
            continue;
        }

        const auto now = Clock::now();
        if (now - txStart > std::chrono::seconds(options_.txTimeoutSec)) {
            Log("ptt: transmit timeout (%d s) - forcing RX", options_.txTimeoutSec);
            txWanted_ = false;
            continue;
        }

        size_t n;
        while ((n = txIn_.Pop(scratch.data(), scratch.size())) > 0) {
            for (size_t i = 0; i < n; ++i) {
                txResampler.Push(scratch[i] * options_.txGain);
            }
        }

        if (!streaming) {
            if (txResampler.Available() < kTxPrefill && now - txStart < kTxStartTimeout) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            streaming = true;
            streamStart = now;
            sent = 0;
        }

        // Pace to the radio's 11520 Hz rate; the link itself carries exactly that.
        // USB-serial writes block for a fixed overhead on top of the wire time, so
        // a late write must be followed by a bigger one or the link never catches up.
        const double due =
            std::chrono::duration<double>(now - streamStart).count() * kRadioTxRate + kTxLeadBytes;
        const size_t count = size_t(std::clamp(due - sent, 0.0, double(txChunk.size())));
        if (count < 64) {
            std::this_thread::sleep_for(2ms);
            continue;
        }
        uint8_t* out = txChunk.data();
        for (size_t i = 0; i < count; ++i) {
            float y;
            if (txResampler.Pop(&y)) {
                out[i] = FloatToU8(y);
                ++txAudio;
            } else {
                out[i] = 128; // underrun: silence
                ++txFiller;
            }
        }
        if (!WriteAll(serialFd_, out, count)) {
            radioDead_ = true;
            break;
        }
        sent += double(count);
        txBytes_ += count;
    }

    if (tx && !radioDead_) {
        SendToRadio(";RX;");
    }
    txActive_ = false;
}

} // namespace trusdx
