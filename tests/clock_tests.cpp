#include <doctest/doctest.h>

#include "sim/WorldClock.h"
#include <cmath>

// The client's estimate of the world's clock (#192). Two clients that started at different
// moments must read the same world time, or the same station is turned two ways.

TEST_CASE("two clients that started apart read the same world time")
{
    WorldClock a, b;
    // The server's clock reads 5000 s. Client A has been running 10 s, client B 300 s.
    a.Observe(5000.0, 10.0);
    b.Observe(5000.0, 300.0);
    // One second later, by each one's own clock.
    CHECK(a.Now(11.0) == doctest::Approx(5001.0));
    CHECK(b.Now(301.0) == doctest::Approx(5001.0));
}

TEST_CASE("jitter is eased, not followed, and never runs the clock backwards")
{
    WorldClock c;
    c.Observe(1000.0, 0.0);
    double prev = c.Now(0.0);
    // Snapshots arriving 0..40 ms late, alternately: the estimate must stay monotonic and
    // within a few tens of milliseconds of the truth.
    for (int i = 1; i <= 600; i++)
    {
        const double local = i / 60.0;
        const double late = (i % 2) ? 0.04 : 0.0;
        c.Observe(1000.0 + local - late, local);
        const double now = c.Now(local);
        CHECK(now >= prev);
        CHECK(std::abs(now - (1000.0 + local)) < 0.05);
        prev = now;
    }
    CHECK(c.Now(10.0) == doctest::Approx(1010.0).epsilon(0.00005));
}

TEST_CASE("a server restart is followed at once")
{
    WorldClock c;
    c.Observe(90000.0, 0.0);
    CHECK(c.Now(1.0) == doctest::Approx(90001.0));
    // A new server starts its clock at zero. Easing toward it at 10% a snapshot would take
    // minutes of parts spinning at the wrong speed.
    c.Observe(0.0, 2.0);
    CHECK(c.Now(3.0) == doctest::Approx(1.0));
}
