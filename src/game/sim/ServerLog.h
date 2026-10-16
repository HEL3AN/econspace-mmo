#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

// What the server says about itself (#400, part of #352): every line it writes while hosting
// carries the time it was written, so a log read the next morning says when a player was
// refused or a region opened, not only that it happened.
//
// A hosting server writes to stderr, stamped. Everything else econserver does -- the batch
// run, the dumps, the self-tests -- writes to stdout as it always has, unstamped: the same
// helpers serve both, and only RunHost turns the stamps on.
namespace Log
{

#if defined(__MINGW_PRINTF_FORMAT)
#define ECON_LOG_FORMAT(a, b) __attribute__((format(__MINGW_PRINTF_FORMAT, a, b)))
#elif defined(__GNUC__)
#define ECON_LOG_FORMAT(a, b) __attribute__((format(printf, a, b)))
#else
#define ECON_LOG_FORMAT(a, b)
#endif

// ISO 8601 in UTC, to the millisecond: "2026-10-16T09:41:07.123Z". UTC because a server and
// the people reading its log are seldom in one time zone, and a log that moves an hour twice
// a year cannot be sorted.
std::string IsoTime(int64_t unixMillis);
// The same, for now.
std::string Now();

// `text` with `stamp` and a space put before every line that starts in it. A line written in
// pieces is stamped once, at its start: `atLineStart` says whether the last call ended one,
// and is updated.
std::string StampLines(const std::string& text, const std::string& stamp, bool& atLineStart);

// On: everything below goes to stderr with a time on each line. Off (the default): Printf
// goes to stdout and Errorf to stderr, both as they are.
void SetStamped(bool on);
bool Stamped();

// printf, through the rules above.
void Printf(const char* fmt, ...) ECON_LOG_FORMAT(1, 2);
// For what has gone wrong: stderr either way.
void Errorf(const char* fmt, ...) ECON_LOG_FORMAT(1, 2);
// The va_list form, for a logger handed one (raylib's).
void VPrintf(bool error, const char* fmt, va_list args);

// The once-a-minute line about how the server is doing: what the world cost to step and what
// went out on the wire. Fed as it runs, read and reset by Line.
class Metrics
{
public:
    // One world tick took `ms` milliseconds.
    void Tick(double ms);
    // Bytes sent to clients so far, in all: the line reports what was added since the last.
    void SetSent(double totalBytes) { sent_ = totalBytes; }

    // "metrics: 3 player(s), 4 connection(s), 2 hot system(s); tick 0.42 ms mean, 1.90 ms
    // max over 3600; out 1234.5 KB (20.6 KB/s)", over the `seconds` since the last line.
    std::string Line(int players, int connections, int hotSystems, double seconds);

private:
    int    ticks_ = 0;
    double tickSum_ = 0.0;
    double tickMax_ = 0.0;
    double sent_ = 0.0;
    double sentAtLast_ = 0.0;
};

}  // namespace Log
