#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace trusdx {

inline void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

inline void Log(const char* fmt, ...)
{
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm t;
    localtime_r(&tv.tv_sec, &t);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", &t);

    std::fprintf(stderr, "%s.%03d ", ts, int(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

} // namespace trusdx
