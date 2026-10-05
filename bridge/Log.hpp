#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/time.h>

namespace trusdx {

// Keep control bytes, quotes and backslashes visible on a single log line.
inline std::string EscapeLogBytes(const void* data, size_t size)
{
    std::string out;
    const auto* bytes = static_cast<const unsigned char*>(data);
    constexpr char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) {
        switch (bytes[i]) {
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default:
            if (bytes[i] >= 0x20 && bytes[i] <= 0x7E) {
                out += char(bytes[i]);
            } else {
                out += "\\x";
                out += hex[bytes[i] >> 4];
                out += hex[bytes[i] & 15];
            }
        }
    }
    return out;
}

inline void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

inline void Log(const char* fmt, ...)
{
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm t;
    localtime_r(&tv.tv_sec, &t);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", &t);

    // Reader, writer and UI threads share stderr; never interleave log lines.
    flockfile(stderr);
    std::fprintf(stderr, "%s.%03d ", ts, int(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    funlockfile(stderr);
}

} // namespace trusdx
