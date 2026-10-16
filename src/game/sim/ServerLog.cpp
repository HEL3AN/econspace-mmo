#include "sim/ServerLog.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

namespace Log
{
namespace
{

std::mutex g_lock;
bool       g_stamped = false;
bool       g_atLineStart = true;  // of the stamped stream: one, since it is all stderr

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm): exact for any day, and
// free of gmtime, which is not thread-safe on one platform and named differently on the other.
void CivilFromDays(int64_t z, int& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t  yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int)(yy + (m <= 2 ? 1 : 0));
}

std::string Format(const char* fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0)
        return std::string();
    std::vector<char> buf((size_t)n + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    return std::string(buf.data(), (size_t)n);
}

void Write(bool error, const std::string& text)
{
    std::lock_guard<std::mutex> g(g_lock);
    if (g_stamped)
    {
        const std::string out = StampLines(text, Now(), g_atLineStart);
        std::fwrite(out.data(), 1, out.size(), stderr);
        std::fflush(stderr);
        return;
    }
    FILE* f = error ? stderr : stdout;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
}

}  // namespace

std::string IsoTime(int64_t unixMillis)
{
    int64_t secs = unixMillis / 1000;
    int     ms = (int)(unixMillis % 1000);
    if (ms < 0)
    {
        ms += 1000;
        secs -= 1;
    }
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0)
    {
        rem += 86400;
        days -= 1;
    }
    int      y = 0;
    unsigned m = 0, d = 0;
    CivilFromDays(days, y, m, d);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y, m, d,
                  (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60), ms);
    return buf;
}

std::string Now()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return IsoTime((int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string StampLines(const std::string& text, const std::string& stamp, bool& atLineStart)
{
    std::string out;
    out.reserve(text.size() + stamp.size() + 1);
    for (const char c : text)
    {
        if (atLineStart)
        {
            out += stamp;
            out += ' ';
            atLineStart = false;
        }
        out += c;
        if (c == '\n')
            atLineStart = true;
    }
    return out;
}

void SetStamped(bool on)
{
    std::lock_guard<std::mutex> g(g_lock);
    g_stamped = on;
    g_atLineStart = true;
}

bool Stamped()
{
    std::lock_guard<std::mutex> g(g_lock);
    return g_stamped;
}

void VPrintf(bool error, const char* fmt, va_list args)
{
    Write(error, Format(fmt, args));
}

void Printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VPrintf(false, fmt, args);
    va_end(args);
}

void Errorf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    VPrintf(true, fmt, args);
    va_end(args);
}

void Metrics::Tick(double ms)
{
    ticks_++;
    tickSum_ += ms;
    tickMax_ = std::max(tickMax_, ms);
}

std::string Metrics::Line(int players, int connections, int hotSystems, double seconds)
{
    const double out = std::max(0.0, sent_ - sentAtLast_);
    char         buf[256];
    std::snprintf(buf, sizeof buf,
                  "metrics: %d player(s), %d connection(s), %d hot system(s); tick %.2f ms mean, "
                  "%.2f ms max over %d; out %.1f KB (%.1f KB/s)",
                  players, connections, hotSystems, ticks_ > 0 ? tickSum_ / ticks_ : 0.0, tickMax_,
                  ticks_, out / 1024.0, seconds > 0.0 ? out / 1024.0 / seconds : 0.0);
    ticks_ = 0;
    tickSum_ = 0.0;
    tickMax_ = 0.0;
    sentAtLast_ = sent_;
    return buf;
}

}  // namespace Log
