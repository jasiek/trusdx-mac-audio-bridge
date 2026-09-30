#pragma once

// truSDX CAT protocol pieces that don't touch hardware.
//
// The radio speaks Kenwood TS-480 CAT at 115200 8N1 plus an audio extension
// (https://dl2man.de/5-trusdx-details/):
//   UA0; / UA1; / UA2;  audio streaming off / on with speaker / on, speaker muted
//   US<u8 samples>;     receive audio, 8-bit unsigned, ~7.8 kHz
//   TX0; US<samples>    transmit, host streams 8-bit audio at 11520 Hz
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
    long v = std::lrintf(x * 127.0f) + 128;
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
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
    Drop,
};

// What to do with a command from the CAT client.
ClientAction ClassifyClientCommand(const std::string& cmd);

// True for a bare query like "FA;" or "IF;".
bool IsQuery(const std::string& cmd);

// Two-letter command name ("FA" for "FA00014074000;").
std::string CommandName(const std::string& frame);

// Rejects frames that are really stray audio bytes.
bool IsPlausibleCatFrame(const std::string& frame);

// Returns an IF reply with its TX/RX status digit set.
std::string WithTxFlag(const std::string& ifReply, bool tx);

} // namespace trusdx
