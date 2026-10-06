#include "Protocol.hpp"

namespace trusdx {

static bool IsFrequencyFrame(const std::string& f)
{
    return f.size() == 14 && f.compare(0, 2, "FA") == 0 && f.back() == ';'
        && f.find_first_not_of("0123456789", 2) == 13;
}

static bool IsModeFrame(const std::string& f)
{
    return f.size() == 4 && f.compare(0, 2, "MD") == 0
        && f[2] >= '1' && f[2] <= '5' && f[3] == ';';
}

ClientAction ClassifyClientCommand(const std::string& cmd)
{
    // ID is local; TX/RX commands preserve the client's PTT variant.
    if (cmd.compare(0, 2, "ID") == 0) {
        return ClientAction::LocalId;
    }
    if (cmd.compare(0, 2, "TX") == 0) {
        return ClientAction::PttOn;
    }
    if (cmd.compare(0, 2, "RX") == 0) {
        return ClientAction::PttOff;
    }
    return ClientAction::Forward;
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

bool IsStatusSetter(const std::string& cmd)
{
    return IsFrequencyFrame(cmd) || IsModeFrame(cmd);
}

std::string CachedStatusReply(const std::string& cmd, const std::string& frequency,
    const std::string& mode, const std::string& info, bool transmitting)
{
    const bool haveFrequency = IsFrequencyFrame(frequency);
    const bool haveMode = IsModeFrame(mode);
    if (cmd == "FA;" && haveFrequency) return frequency;
    if (cmd == "MD;" && haveMode) return mode;
    if (cmd == "IF;" && haveFrequency && haveMode && info.size() == 38
        && info.compare(0, 2, "IF") == 0 && IsPlausibleCatFrame(info)) {
        std::string reply = info;
        reply.replace(2, 11, frequency, 2, 11);
        reply[28] = transmitting ? '1' : '0'; // Bridge-commanded Kenwood IF PTT field.
        reply[29] = mode[2];
        return reply;
    }
    return "?;";
}

} // namespace trusdx
