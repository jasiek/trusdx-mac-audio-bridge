#include "Protocol.hpp"

namespace trusdx {

ClientAction ClassifyClientCommand(const std::string& cmd)
{
    // Hamlib sends TX; / TX0; (mic) / TX1; (data) for PTT; the truSDX keys on TX0;
    // and needs the audio stream opened, so all of them go through the bridge.
    // TX2; (tune) carries no audio and goes to the radio as-is.
    if (cmd == "TX;" || cmd == "TX0;" || cmd == "TX1;") {
        return ClientAction::PttOn;
    }
    if (cmd == "RX;") {
        return ClientAction::PttOff;
    }
    // The bridge owns audio streaming; a client toggling it would break the stream.
    if (cmd.compare(0, 2, "UA") == 0 || cmd.compare(0, 2, "US") == 0) {
        return ClientAction::Drop;
    }
    return ClientAction::Forward;
}

bool IsQuery(const std::string& cmd)
{
    return cmd.size() == 3 && cmd[2] == ';';
}

std::string CommandName(const std::string& frame)
{
    return frame.substr(0, 2);
}

bool IsPlausibleCatFrame(const std::string& frame)
{
    if (frame.size() < 2 || frame.back() != ';') {
        return false;
    }
    const char first = frame[0];
    if (!((first >= 'A' && first <= 'Z') || first == '?')) {
        return false;
    }
    for (char c : frame) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

std::string WithTxFlag(const std::string& ifReply, bool tx)
{
    // TS-480 IF reply: "IF" freq(11) step(5) rit(5) ritOn xitOn bank ch(2) TX/RX mode ...
    constexpr size_t kTxFlag = 2 + 11 + 5 + 5 + 1 + 1 + 1 + 2; // Hamlib reads info[28]
    std::string out = ifReply;
    if (out.compare(0, 2, "IF") == 0 && out.size() > kTxFlag + 1) {
        out[kTxFlag] = tx ? '1' : '0';
    }
    return out;
}

} // namespace trusdx
