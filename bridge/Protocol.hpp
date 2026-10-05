#pragma once

// truSDX CAT protocol pieces that don't touch hardware.
//
// The radio speaks Kenwood TS-480 CAT at 115200 8N1 plus an audio extension
// (https://dl2man.de/5-trusdx-details/):
//   UA0; / UA1; / UA2;  audio streaming off / on with speaker / on, speaker muted
//   US<u8 samples>;     receive audio, 8-bit unsigned, ~7.8 kHz
//   TX0; <samples>      transmit, host streams 8-bit audio at 11520 Hz
//   ;RX;                end the stream and return to receive
// A ';' inside the audio would end the stream, so samples never use 0x3B.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace trusdx {

constexpr double kRadioTxRate = 11520.0; // exactly 115200 baud / 10 bits per byte
constexpr double kRadioRxRateNominal = 7812.5;

inline float U8ToFloat(uint8_t v)
{
    return (int(v) - 128) / 128.0f;
}

inline uint8_t FloatToU8(float x)
{
    // Reference: capture signed 16-bit PCM, then 128 + sample // 256.
    const float pcm = std::fmax(-32768.0f, std::fmin(32767.0f, x * 32768.0f));
    const long v = long(std::floor(std::trunc(pcm) / 256.0f)) + 128;
    return v == ';' ? uint8_t(':') : uint8_t(v);
}

// Splits bytes from the radio into CAT reply frames and receive-audio samples.
class RadioStreamParser {
public:
    template <typename OnAudio, typename OnFrame>
    void Feed(const uint8_t* p, size_t n, OnAudio&& onAudio, OnFrame&& onFrame)
    {
        size_t i = 0;
        while (i < n) {
            if (audio_) {
                size_t j = i;
                while (j < n && p[j] != ';') {
                    ++j;
                }
                if (j > i) {
                    onAudio(p + i, j - i);
                }
                if (j < n) {
                    audio_ = false; // ';' closes the stream
                    ++j;
                }
                i = j;
                continue;
            }

            const char c = char(p[i++]);
            if (c == ';') {
                if (!frame_.empty()) {
                    onFrame(frame_ + ';');
                }
                frame_.clear();
            } else {
                frame_.push_back(c);
                if (frame_ == "US") {
                    audio_ = true;
                    frame_.clear();
                } else if (frame_.size() > kMaxFrame) {
                    frame_.clear(); // audio we joined mid-stream, or line noise
                }
            }
        }
    }

    void Reset()
    {
        audio_ = false;
        frame_.clear();
    }

private:
    static constexpr size_t kMaxFrame = 64;
    bool audio_ = false;
    std::string frame_;
};

// Splits the byte stream from the CAT client into ';'-terminated commands.
class CatSplitter {
public:
    template <typename OnCommand>
    void Feed(const char* p, size_t n, OnCommand&& onCommand)
    {
        for (size_t i = 0; i < n; ++i) {
            const char c = p[i];
            if (c == '\r' || c == '\n') {
                continue;
            }
            buf_.push_back(c);
            if (c == ';') {
                if (buf_.size() > 1) {
                    onCommand(buf_);
                }
                buf_.clear();
            } else if (buf_.size() > 256) {
                buf_.clear();
            }
        }
    }

private:
    std::string buf_;
};

enum class ClientAction {
    Forward,
    PttOn,
    PttOff,
    LocalId,
};

// What to do with a command from the CAT client.
ClientAction ClassifyClientCommand(const std::string& cmd);

// Two-letter command name ("FA" for "FA00014074000;").
std::string CommandName(const std::string& frame);

// Rejects frames that are really stray audio bytes.
bool IsPlausibleCatFrame(const std::string& frame);

// Status from confirmed RX state, with the bridge's commanded PTT.
// Unsupported commands and missing state return ?; without touching serial audio.
std::string CachedStatusReply(const std::string& cmd, const std::string& frequency,
    const std::string& mode, const std::string& info, bool transmitting);

} // namespace trusdx
