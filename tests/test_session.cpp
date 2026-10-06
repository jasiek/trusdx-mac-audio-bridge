// Exercise the real Bridge writer/reader against a PTY, with no radio or Core Audio I/O.
#include "Bridge.hpp"
#include "Config.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace trusdx;
static SpscRing<float>* capture;
static std::atomic<bool> slowSerialDrain{false};

// Emulate the macOS driver returning a radio reply while its drain still waits.
// The next client query must reach the radio without waiting for that drain.
extern "C" int tcdrain(int)
{
    if (slowSerialDrain) std::this_thread::sleep_for(600ms);
    return 0;
}

// Controlled capture: tests feed samples explicitly; never open an audio device.
namespace trusdx {
AudioLink::AudioLink(SpscRing<uint8_t>& rx, SpscRing<float>& tx,
    const std::atomic<bool>& wanted, double rate)
    : rxRaw_(rx), txIn_(tx), txWanted_(wanted), rxRate_(rate),
      nominalStep_(rate / kDeviceSampleRate), rxResampler_(nominalStep_)
{
    capture = &tx;
}
AudioLink::~AudioLink() = default;
bool AudioLink::Start() { return true; }
void AudioLink::Stop() {}
bool AudioLink::Healthy() const { return true; }
double AudioLink::EstimatedRxRate() const { return rxRate_; }
} // namespace trusdx

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static std::string ReadBytes(int fd, size_t count, int timeoutMs = 2000)
{
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < count && std::chrono::steady_clock::now() < deadline) {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        char buf[4096];
        const ssize_t n = read(fd, buf, std::min(sizeof buf, count - out.size()));
        if (n > 0) out.append(buf, size_t(n));
    }
    return out;
}

static void Expect(int fd, const std::string& expected, int timeoutMs = 2000)
{
    const std::string actual = ReadBytes(fd, expected.size(), timeoutMs);
    if (actual != expected) {
        throw std::runtime_error("wire mismatch: expected " + expected + ", got " + actual);
    }
}

static void ExpectQuiet(int fd)
{
    pollfd p = {fd, POLLIN, 0};
    Check(poll(&p, 1, 100) == 0, "unexpected bytes (US, filler, duplicate PTT or UA)");
}

int main()
{
    char directory[] = "/tmp/trusdx-session-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "mkdtemp failed");
    int master, slave;
    char serialPath[128];
    Check(openpty(&master, &slave, serialPath, nullptr, nullptr) == 0, "openpty failed");
    termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fcntl(master, F_SETFL, O_NONBLOCK);

    Options options;
    options.port = serialPath; // Always the fake PTY, never auto-detect real hardware.
    options.catLink = std::string(directory) + "/cat";
    options.txTimeoutSec = 3;
    std::atomic<bool> stop{false};
    int failures = 0;
    std::atomic<bool> drainStop{false};
    std::thread shutdownDrainer;
    {
        Bridge bridge(options);
        std::thread worker([&] { bridge.Run(stop); });
        int client = -1;
        try {
            // The same initialization as the reference, plus our startup unkey.
            Expect(master, ";RX;MD2;UA2;", 5000);
            Check(WriteAll(master, "US\x80\x81", 4), "fake RX write failed");
            const std::string info = "IF0001407400000000+000000000020000000;";
            Expect(master, "FA;");
            WriteAll(master, ";FA00014074000;US\x80\x81", 19);
            Expect(master, "MD;");
            WriteAll(master, ";MD2;US\x80\x81", 9);
            Expect(master, "IF;");
            const std::string state = ";" + info + "US\x80\x81";
            WriteAll(master, state.data(), state.size());
            client = open(options.catLink.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            Check(client >= 0, "CAT client open failed");
            ExpectQuiet(client); // Startup status replies never reach the client.

            WriteAll(client, "ID;", 3);
            Expect(client, "ID020;");
            ExpectQuiet(master); // ID is answered locally in RX too.

            WriteAll(client, "TX1;", 4);
            Expect(master, "TX1;"); // Forward the original PTT variant unchanged.
            ExpectQuiet(master); // No samples captured: no artificial silence.

            WriteAll(client, "RM;", 3);
            Expect(client, "?;"); // Unsupported query stays off RF.
            ExpectQuiet(master);

            WriteAll(client, "FA;MD;", 6);
            Expect(client, "FA00014074000;MD2;");
            ExpectQuiet(master);

            WriteAll(client, "IF;", 3);
            std::string txInfo = info;
            txInfo[28] = '1';
            Expect(client, txInfo); // Full IF reply, with commanded TX state.
            ExpectQuiet(master);

            WriteAll(client, "FA;FA;FA;", 9);
            Expect(client, "FA00014074000;FA00014074000;FA00014074000;");
            WriteAll(client, "ID;", 3);
            Expect(client, "ID020;"); // ID stays local during TX too.
            ExpectQuiet(master);

            const std::string blocked = "FA00014075000;UA0;TX0;";
            WriteAll(client, blocked.data(), blocked.size());
            Expect(client, "?;?;"); // Reject setters; repeated TX is a no-op.
            ExpectQuiet(master);

            // Capture reset discards the first 512 resampled samples. Each 3072
            // samples at 48 kHz yields ~737 samples at the radio's 11520 Hz rate.
            std::vector<float> input(3072, 0.251f);
            Check(capture->Push(input.data(), input.size()) == input.size(), "capture full");
            ExpectQuiet(master);

            // Polling while real audio is queued must never enter the serial stream.
            WriteAll(client, "FA;MD;IF;RM;", 12);
            Check(capture->Push(input.data(), input.size()) == input.size(), "capture full");
            Expect(client, "FA00014074000;MD2;" + txInfo + "?;");
            Expect(master, std::string(512, char(160)));
            ExpectQuiet(master);

            // Reset immediately, even if fresh RX samples have just arrived.
            WriteAll(master, "\x80\x81", 2);
            const std::string toggles = "RX;RX;IF;FA;MD;TX0;RX;";
            const auto unkeyAt = std::chrono::steady_clock::now();
            WriteAll(client, toggles.data(), toggles.size());
            // Unkey deliberately sleeps for 60 + 10 ms. Shared CI runners can
            // oversleep these timers, so allow scheduling headroom while still
            // requiring cached replies before the 500 ms recovery sample wait.
            Expect(master, ";RX;", 400);
            Expect(client, info + "FA00014074000;MD2;", 150);
            // These replies must arrive during the reset, not after its delays.
            Check(std::chrono::steady_clock::now() - unkeyAt < 450ms, "CAT stalled during reset");
            Expect(master, "UA0;", 150);
            WriteAll(master, ";?;UA0;", 7);
            Expect(master, "UA2;", 350);
            WriteAll(master, "UA2;", 4);
            Expect(master, "RX;", 150);
            WriteAll(master, "US\x80\x81", 4);
            ExpectQuiet(client); // Internal reset replies cannot poison CAT.

            // The queued TX/RX happens only after recovery, and starts a new reset.
            Expect(master, "TX0;;RX;UA0;", 250);
            WriteAll(master, ";UA0;", 5);
            Expect(master, "UA2;", 350);
            WriteAll(master, "UA2;", 4);
            Expect(master, "RX;", 150);
            // Deliberately withhold samples: the immediate attempt must time out.
            WriteAll(client, "IF;", 3);
            Expect(client, info, 150); // RX flag, even during the wait for samples.
            ExpectQuiet(client);

            // The watchdog retries a failed immediate reset after five seconds.
            Expect(master, "RX;", 5500);
            WriteAll(master, "?;", 2);
            Expect(master, "UA0;", 150);
            WriteAll(master, "UA0;", 4);
            Expect(master, "UA2;", 350);
            WriteAll(master, "UA2;", 4);
            Expect(master, "RX;", 150);
            WriteAll(master, "US\x80\x81", 4);
            ExpectQuiet(client);
            ExpectQuiet(master);

            WriteAll(client, "RX;", 3);
            Expect(master, "RX;");
            ExpectQuiet(master); // Repeated RX while already receiving is not a reset.

            slowSerialDrain = true;
            WriteAll(client, "IF;", 3);
            Expect(master, "IF;", 150);
            const std::string liveInfo = ";" + info;
            WriteAll(master, liveInfo.data(), liveInfo.size());
            Expect(client, info, 150);
            // Hamlib sends FA as soon as IF arrives. A drain after the IF write
            // used to hold this FA despite IF having already completed.
            WriteAll(client, "FA;", 3);
            Expect(master, "FA;", 150); // Must not wait for the simulated 600 ms drain.
            WriteAll(master, ";FA00014075000;", 15);
            Expect(client, "FA00014075000;", 150);
            slowSerialDrain = false;

            // Continued RX samples suppress further recovery probes.
            const std::string stream = "US" + std::string(12000, char(128));
            WriteAll(master, stream.data(), stream.size());

            WriteAll(client, "UA1;", 4);
            Expect(master, "UA1;"); // Match the reference's transparent forwarding.
            WriteAll(client, "TX0;", 4);
            Expect(master, "TX0;");
            WriteAll(client, "FA;IF;", 6);
            txInfo.replace(2, 11, "00014075000");
            Expect(client, "FA00014075000;" + txInfo);
            ExpectQuiet(master);
            Expect(master, ";RX;", 4000); // Timeout also triggers immediate recovery.
            Expect(master, "UA0;", 150);
            WriteAll(master, ";UA0;", 5);
            Expect(master, "UA2;", 350);
            WriteAll(master, "UA2;", 4);
            Expect(master, "RX;", 150);
            WriteAll(master, "US\x80\x81", 4);
            ExpectQuiet(client);
            ExpectQuiet(master);

            // Speaker toggle switches UA mode in place via a serial audio reset.
            bridge.SetSpeaker(true);
            Expect(master, "RX;", 150);
            Expect(master, "UA0;", 150);
            WriteAll(master, ";UA0;", 5);
            Expect(master, "UA1;", 350);
            WriteAll(master, "UA1;", 4);
            Expect(master, "RX;", 150);
            WriteAll(master, "US\x80\x81", 4);
            ExpectQuiet(client); // Mode-change acknowledgements stay internal.
            ExpectQuiet(master);
            bridge.SetSpeaker(true);
            ExpectQuiet(master); // Unchanged mode: no reset.
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s\n", e.what());
            failures = 1;
        }
        stop = true;
        worker.join();
        // Consume shutdown commands while the Bridge destructor sends RX;UA0;.
        shutdownDrainer = std::thread([&] {
            while (!drainStop) ReadBytes(master, 1, 50);
        });
        if (client >= 0) close(client);
    }
    drainStop = true;
    shutdownDrainer.join();
    close(master);
    close(slave);
    rmdir(directory);
    if (!failures) std::puts("serial session tests passed (fake radio, no Core Audio I/O)");
    return failures;
}
