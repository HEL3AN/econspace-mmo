#pragma once

// The client's estimate of the server's clock (#192).
//
// Anything that moves is a function of time (#136), so two players see one station turned
// the same way only if they agree on the time, not just on how fast it passes. Each
// snapshot says what the world's clock read when it was built; this keeps an offset from
// the local clock, eased toward each new reading so that network jitter does not make
// rotating parts stutter, and never lets the estimate run backwards.
//
// Latency is not subtracted: a reading is late by the trip, so every client runs behind
// the server by its own latency -- tens of milliseconds, invisible on a part that takes a
// minute to turn. Header-only and free of raylib, so a test can drive it with made-up
// times.
class WorldClock
{
public:
    // Share of the gap closed per snapshot. Small, because a single late snapshot is noise.
    static constexpr double EASE = 0.1;
    // A gap this large is not jitter: the server restarted, or the first readings are in.
    // Jump rather than ease.
    static constexpr double JUMP = 2.0;

    void Observe(double serverTime, double localNow)
    {
        const double measured = serverTime - localNow;
        if (!synced_ || measured - offset_ > JUMP || offset_ - measured > JUMP)
        {
            offset_ = measured;
            synced_ = true;
            return;
        }
        offset_ += (measured - offset_) * EASE;
    }

    // The world's time now. Monotonic: easing toward an earlier reading slows the clock
    // instead of turning it back, which would spin every part backwards for a moment. A
    // jump backwards (a server restart) is the exception and is taken as it comes.
    double Now(double localNow)
    {
        double t = localNow + offset_;
        if (t < last_ && last_ - t < JUMP)
            t = last_;
        last_ = t;
        return t;
    }

    bool Synced() const { return synced_; }

private:
    bool   synced_ = false;
    double offset_ = 0.0;
    double last_ = 0.0;
};
