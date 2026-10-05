// Unit tests for the hardware-independent parts. Run: ctest --test-dir build

#include "Protocol.hpp"
#include "Resampler.hpp"
#include "TimelineRing.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace trusdx;

static int gFailures = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++gFailures;                                                        \
        }                                                                       \
    } while (0)

static void TestParserSplitsAudioAndFrames()
{
    // Byte stream as captured from the radio, fed in every possible split.
    static const char kWire[] = "garbage\x85UA2;US\x85\x87\x55\x53;FA00014074000;US\x80\x81";
    const std::string wire(kWire, sizeof kWire - 1);
    for (size_t cut = 0; cut <= wire.size(); ++cut) {
        RadioStreamParser parser;
        std::vector<uint8_t> audio;
        std::vector<std::string> frames;
        auto onAudio = [&](const uint8_t* p, size_t n) { audio.insert(audio.end(), p, p + n); };
        auto onFrame = [&](const std::string& f) { frames.push_back(f); };
        const auto* bytes = reinterpret_cast<const uint8_t*>(wire.data());
        parser.Feed(bytes, cut, onAudio, onFrame);
        parser.Feed(bytes + cut, wire.size() - cut, onAudio, onFrame);

        const std::vector<uint8_t> expectAudio = {0x85, 0x87, 0x55, 0x53, 0x80, 0x81};
        CHECK(audio == expectAudio);
        CHECK(frames.size() == 2);
        if (frames.size() == 2) {
            CHECK(!IsPlausibleCatFrame(frames[0])); // "garbage\x85UA2;" -> rejected
            CHECK(frames[1] == "FA00014074000;");
            CHECK(IsPlausibleCatFrame(frames[1]));
        }
    }
}

static void TestClientClassification()
{
    CHECK(ClassifyClientCommand("TX;") == ClientAction::PttOn);
    CHECK(ClassifyClientCommand("TX0;") == ClientAction::PttOn);
    CHECK(ClassifyClientCommand("TX1;") == ClientAction::PttOn);
    CHECK(ClassifyClientCommand("RX;") == ClientAction::PttOff);
    CHECK(ClassifyClientCommand("TX2;") == ClientAction::PttOn);
    CHECK(ClassifyClientCommand("UA1;") == ClientAction::Forward);
    CHECK(ClassifyClientCommand("FA00007074000;") == ClientAction::Forward);
    CHECK(ClassifyClientCommand("ID;") == ClientAction::LocalId);
    CHECK(ClassifyClientCommand("US;") == ClientAction::Forward);
    CHECK(ClassifyClientCommand("RM;") == ClientAction::Forward);

    CatSplitter splitter;
    std::vector<std::string> cmds;
    splitter.Feed("FA;I", 4, [&](const std::string& c) { cmds.push_back(c); });
    splitter.Feed("F;\r\n;", 5, [&](const std::string& c) { cmds.push_back(c); });
    CHECK((cmds == std::vector<std::string>{"FA;", "IF;"}));
}

static void TestSampleConversion()
{
    for (int v = 0; v < 256; ++v) {
        const uint8_t back = FloatToU8(U8ToFloat(uint8_t(v)));
        CHECK(back != ';');
        CHECK(std::abs(int(back) - v) <= 1 || (v == ';' && back == ':'));
    }
    CHECK(FloatToU8(2.0f) == 255);
    CHECK(FloatToU8(-2.0f) == 0);
    for (int pcm = -32768; pcm <= 32767; ++pcm) {
        int expected = int(std::floor(pcm / 256.0)) + 128;
        if (expected == ';') expected = ':';
        CHECK(FloatToU8(pcm / 32768.0f) == expected);
    }
}

// Frequency (Hz) and RMS of a signal, via zero crossings.
static void Measure(const std::vector<float>& x, double rate, double* freq, double* rms)
{
    const size_t skip = x.size() / 10; // ignore filter start-up
    int crossings = 0;
    double sum = 0;
    for (size_t i = skip + 1; i < x.size(); ++i) {
        if ((x[i - 1] < 0) != (x[i] < 0)) {
            ++crossings;
        }
        sum += double(x[i]) * x[i];
    }
    const double seconds = double(x.size() - skip - 1) / rate;
    *freq = crossings / 2.0 / seconds;
    *rms = std::sqrt(sum / double(x.size() - skip - 1));
}

static std::vector<float> RunResampler(double inRate, double outRate, double toneHz, size_t inCount)
{
    Resampler rs(inRate / outRate);
    std::vector<float> out;
    for (size_t i = 0; i < inCount; ++i) {
        rs.Push(float(0.5 * std::sin(2 * M_PI * toneHz * double(i) / inRate)));
        float y;
        while (rs.Pop(&y)) {
            out.push_back(y);
        }
    }
    return out;
}

static void TestResamplerRx()
{
    const double in = 7812.5, outRate = 48000;
    const auto y = RunResampler(in, outRate, 1000, 78125); // 10 s
    CHECK(std::fabs(double(y.size()) - 480000) < 200);
    double f, rms;
    Measure(y, outRate, &f, &rms);
    CHECK(std::fabs(f - 1000) < 1);
    CHECK(std::fabs(rms - 0.5 / std::sqrt(2)) < 0.01);
}

static void TestResamplerTx()
{
    const double in = 48000, outRate = 11520;
    double f, rms;

    const auto pass = RunResampler(in, outRate, 1500, 480000);
    CHECK(std::fabs(double(pass.size()) - 115200) < 200);
    Measure(pass, outRate, &f, &rms);
    CHECK(std::fabs(f - 1500) < 1);
    CHECK(std::fabs(rms - 0.5 / std::sqrt(2)) < 0.01);

    // 8 kHz would alias to 3.52 kHz; it must be filtered out first.
    const auto stop = RunResampler(in, outRate, 8000, 480000);
    Measure(stop, outRate, &f, &rms);
    CHECK(rms < 0.5 / std::sqrt(2) * 0.01); // > 40 dB down
}

static void TestTimelineRing()
{
    static TimelineRing ring;
    float src[10];
    for (int i = 0; i < 10; ++i) {
        src[i] = float(i + 1);
    }
    ring.Write(100000, src, 10);

    float dst[20];
    ring.Read(99995, dst, 20);
    for (int i = 0; i < 20; ++i) {
        const int f = 99995 + i;
        const float expect = (f >= 100000 && f < 100010) ? float(f - 100000 + 1) : 0.0f;
        CHECK(dst[i] == expect);
    }

    // After a gap the skipped frames are silence, not whatever was there before.
    ring.Write(100000 + TimelineRing::kFrames + 5, src, 10);
    ring.Read(100000 + TimelineRing::kFrames - 2, dst, 7);
    for (int i = 0; i < 7; ++i) {
        CHECK(dst[i] == 0.0f);
    }
}

int main()
{
    TestParserSplitsAudioAndFrames();
    TestClientClassification();
    const std::string info = "IF0001407400000000+000000000020000000;";
    CHECK(CachedStatusReply("FA;", "FA00014074000;", "MD2;", info, true) == "FA00014074000;");
    CHECK(CachedStatusReply("FA;", "FA0001x074000;", "MD2;", info, true) == "?;");
    CHECK(CachedStatusReply("FA;", "", "MD2;", info, true) == "?;");
    CHECK(CachedStatusReply("MD;", "FA00014074000;", "MD2;", info, true) == "MD2;");
    std::string txInfo = info;
    txInfo.replace(2, 11, "00014075000");
    txInfo[28] = '1';
    txInfo[29] = '1';
    CHECK(CachedStatusReply("IF;", "FA00014075000;", "MD1;", info, true) == txInfo);
    CHECK(CachedStatusReply("IF;", "FA00014074000;", "MD2;", "IFbad;", true) == "?;");
    CHECK(CachedStatusReply("RM;", "FA00014074000;", "MD2;", info, true) == "?;");
    CHECK(CachedStatusReply("FA00014075000;", "FA00014074000;", "MD2;", info, true) == "?;");
    CHECK(CachedStatusReply("IF;", "FA00014074000;", "MD2;", info, false) == info);
    TestSampleConversion();
    TestResamplerRx();
    TestResamplerTx();
    TestTimelineRing();
    if (gFailures == 0) {
        std::printf("all tests passed\n");
    }
    return gFailures == 0 ? 0 : 1;
}
