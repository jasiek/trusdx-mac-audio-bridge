#include "Ports.hpp"
#include "Log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

namespace trusdx {

int OpenSerial(const std::string& path, std::string* error)
{
    const int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        *error = std::strerror(errno);
        return -1;
    }
    auto fail = [&](const char* what) {
        *error = std::string(what) + ": " + std::strerror(errno);
        close(fd);
        return -1;
    };

    // Keep other programs (e.g. WSJT-X still configured for the radio) off the port.
    if (ioctl(fd, TIOCEXCL) < 0) {
        return fail("TIOCEXCL");
    }

    termios t;
    if (tcgetattr(fd, &t) < 0) {
        return fail("tcgetattr");
    }
    cfmakeraw(&t);
    t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetspeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        return fail("tcsetattr");
    }

    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK) < 0) {
        return fail("fcntl");
    }

    int dtr = TIOCM_DTR;
    int rts = TIOCM_RTS;
    ioctl(fd, TIOCMBIS, &dtr);
    ioctl(fd, TIOCMBIC, &rts);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

std::string FindRadioPort()
{
    for (const char* pattern : {"/dev/cu.wchusbserial*", "/dev/cu.usbserial*"}) {
        glob_t g;
        if (glob(pattern, 0, nullptr, &g) == 0 && g.gl_pathc > 0) {
            std::string path = g.gl_pathv[0];
            globfree(&g);
            return path;
        }
        globfree(&g);
    }
    return {};
}

bool WriteAll(int fd, const void* data, size_t len)
{
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        p += n;
        len -= size_t(n);
    }
    return true;
}

CatPty::~CatPty()
{
    if (!link_.empty()) {
        unlink(link_.c_str());
    }
    if (slave_ >= 0) {
        close(slave_);
    }
    if (master_ >= 0) {
        close(master_);
    }
}

bool CatPty::Open(const std::string& linkPath, std::string* error)
{
    char name[128];
    if (openpty(&master_, &slave_, name, nullptr, nullptr) < 0) {
        *error = std::string("openpty: ") + std::strerror(errno);
        return false;
    }

    termios t;
    tcgetattr(slave_, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    tcsetattr(slave_, TCSANOW, &t);
    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);

    unlink(linkPath.c_str());
    if (symlink(name, linkPath.c_str()) < 0) {
        *error = "symlink " + linkPath + ": " + std::strerror(errno);
        return false;
    }
    link_ = linkPath;
    return true;
}

ssize_t CatPty::Read(void* data, size_t size)
{
    const ssize_t count = read(master_, data, size);
    if (count > 0 && verbose_) {
        Log("virtual CAT <- client (%zd bytes) \"%s\"", count,
            EscapeLogBytes(data, size_t(count)).c_str());
    }
    return count;
}

void CatPty::Write(const std::string& data)
{
    if (master_ >= 0) {
        const ssize_t count = write(master_, data.data(), data.size());
        const int error = errno;
        if (verbose_) {
            if (count < 0) {
                Log("virtual CAT -> client FAILED (%zu bytes) \"%s\": %s",
                    data.size(), EscapeLogBytes(data.data(), data.size()).c_str(),
                    std::strerror(error));
            } else {
                Log("virtual CAT -> client (%zd/%zu bytes) \"%s\"", count, data.size(),
                    EscapeLogBytes(data.data(), size_t(count)).c_str());
                if (size_t(count) < data.size()) {
                    Log("virtual CAT: dropped %zu reply bytes after partial write",
                        data.size() - size_t(count));
                }
            }
        }
    }
}

} // namespace trusdx
