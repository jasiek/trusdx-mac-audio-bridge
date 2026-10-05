#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <sys/types.h>

namespace trusdx {

// Opens the radio's USB serial port: 115200 8N1 raw, exclusive, DTR high, RTS low
// (RTS high would key the transmitter). Returns -1 and sets `error` on failure.
int OpenSerial(const std::string& path, std::string* error);

// First /dev/cu.wchusbserial* (CH340, as in the truSDX) or /dev/cu.usbserial*.
std::string FindRadioPort();

// Writes all bytes to a blocking fd. Returns false on error.
bool WriteAll(int fd, const void* data, size_t len);

// Pseudo-terminal the CAT client (Hamlib in WSJT-X) opens instead of the radio,
// reachable through a stable symlink such as /tmp/trusdx-cat.
class CatPty {
public:
    ~CatPty();

    bool Open(const std::string& linkPath, std::string* error);
    int Fd() const { return master_; }
    void SetVerbose(bool enabled) { verbose_ = enabled; }
    ssize_t Read(void* data, size_t size);

    // Non-blocking; replies are dropped if nobody is reading the port.
    void Write(const std::string& data);

private:
    int master_ = -1;
    int slave_ = -1; // held open so the master never sees hang-up between clients
    std::string link_;
    std::atomic<bool> verbose_{false};
};

} // namespace trusdx
