#include <doctest/doctest.h>

#include "core/Route.h"
#include "sim/Protocol.h"

#include <algorithm>
#include <string>
#include <vector>

// The one route planner (#432): a map preview and a route order are the same search.

namespace
{
using Path = std::vector<std::string>;

// a - c - e - d and a - b - d: two ways from a to d, one hop apart. Security per system.
WorldLoader::Universe Map()
{
    WorldLoader::Universe                            u;
    const std::vector<std::pair<std::string, float>> systems = {
        { "a", 1.0f }, { "b", 0.2f }, { "c", 0.9f },
        { "d", 0.8f }, { "e", 0.5f }, { "island", 1.0f }
    };
    for (const auto& [id, sec] : systems)
    {
        WorldLoader::SystemInfo s;
        s.id = s.name = id;
        s.security = sec;
        s.mapPos = { 0.0f, 0.0f };  // where it is on the map never decides a jump
        u.systems.push_back(s);
    }
    u.links = { { "a", "c" }, { "c", "e" }, { "e", "d" }, { "a", "b" }, { "b", "d" } };
    return u;
}
}  // namespace

TEST_CASE("a route is the fewest known jumps, and ties go the same way whatever the order")
{
    WorldLoader::Universe u = Map();
    CHECK(Route::Find(u, "a", "d", 10, 0).path == Path{ "a", "b", "d" });
    CHECK(Route::Find(u, "a", "a", 10, 0).path == Path{ "a" });
    CHECK_FALSE(Route::Find(u, "a", "island", 10, 0).reason.empty());
    CHECK(Route::Find(u, "a", "missing", 10, 0).path.empty());

    // Two equal ways: the lower id wins, with the links in either order.
    u.links.push_back({ "a", "f" });
    u.links.push_back({ "f", "d" });
    WorldLoader::SystemInfo f;
    f.id = "f";
    u.systems.push_back(f);
    CHECK(Route::Find(u, "a", "d", 10, 0).path == Path{ "a", "b", "d" });
    std::reverse(u.links.begin(), u.links.end());
    std::reverse(u.systems.begin(), u.systems.end());
    CHECK(Route::Find(u, "a", "d", 10, 0).path == Path{ "a", "b", "d" });
    CHECK(Route::Find(u, "d", "a", 10, 0).path == Path{ "d", "b", "a" });

    // A link to a system the index does not hold is no way through it.
    u.links = { { "a", "secret" }, { "secret", "d" } };
    CHECK(Route::Find(u, "a", "d", 10, 0).path.empty());
}

TEST_CASE("a route keeps to high or low security as asked, and takes the only way when it must")
{
    const WorldLoader::Universe u = Map();
    // b is 0.2: the short way goes through it, the safer way round it.
    CHECK(Route::Find(u, "a", "d", 0, 0, Route::Preference::Shortest).path ==
          Path{ "a", "b", "d" });
    CHECK(Route::Find(u, "a", "d", 0, 0, Route::Preference::Safer).path ==
          Path{ "a", "c", "e", "d" });
    CHECK(Route::Find(u, "a", "d", 0, 0, Route::Preference::LessSecure).path ==
          Path{ "a", "b", "d" });
    // To b itself there is no high-security way: the safer route still gets there.
    CHECK(Route::Find(u, "a", "b", 0, 0, Route::Preference::Safer).path == Path{ "a", "b" });

    // What the map shows as 0.5 is high security, and what it shows as 0.4 is not.
    CHECK(Route::HighSec(0.46f));
    CHECK(Route::HighSec(0.5f));
    CHECK_FALSE(Route::HighSec(0.44f));
    for (int p = 0; p <= Route::PREFERENCE_MAX; ++p)
    {
        Route::Preference back;
        REQUIRE(Route::PreferenceFromName(Route::PreferenceName((Route::Preference)p), back));
        CHECK((int)back == p);
    }
    Route::Preference none;
    CHECK_FALSE(Route::PreferenceFromName("fastest", none));
}

TEST_CASE("a route through waypoints is its legs joined, in the order given")
{
    const WorldLoader::Universe u = Map();
    const Route::Result r = Route::FindVia(u, "a", { "e", "b" }, 0, 0, Route::Preference::Shortest);
    CHECK(r.path == Path{ "a", "c", "e", "d", "b" });
    // A waypoint the ship is at is passed; one repeated is visited once.
    CHECK(Route::FindVia(u, "a", { "a", "d" }, 0, 0, Route::Preference::Shortest).path ==
          Path{ "a", "b", "d" });
    CHECK(Route::FindVia(u, "a", { "d", "d" }, 0, 0, Route::Preference::Shortest).path ==
          Path{ "a", "b", "d" });
    // A leg that cannot be flown fails the whole route, and says why.
    const Route::Result cut =
        Route::FindVia(u, "a", { "island", "d" }, 0, 0, Route::Preference::Shortest);
    CHECK(cut.path.empty());
    CHECK(cut.reason.find("island") != std::string::npos);
}

TEST_CASE("a route goes round a crossing that has closed or that the ship is too heavy for")
{
    WorldLoader::Universe u = Map();
    u.links[3].kind = "mouth";  // a - b
    u.links[3].maxMass = 20;
    u.links[3].closesAt = 100;
    CHECK(Route::Find(u, "a", "d", 10, 99).path.size() == 3);
    CHECK(Route::Find(u, "a", "d", 30, 99).path.size() == 4);   // too heavy: the long way
    CHECK(Route::Find(u, "a", "d", 10, 100).path.size() == 4);  // closed: the long way
    u.links.erase(u.links.begin());                             // and no long way
    const Route::Result heavy = Route::Find(u, "a", "d", 30, 99);
    CHECK(heavy.path.empty());
    CHECK(heavy.reason.find("too heavy") != std::string::npos);
    CHECK(Route::Find(u, "a", "d", 10, 100).reason.find("closed") != std::string::npos);
}

TEST_CASE("the index carries a crossing's limits, so the map plans as the server does")
{
    WorldLoader::Universe u = Map();
    u.links.front().kind = "mouth";
    u.links.front().maxMass = 30;
    u.links.front().closesAt = 100;
    WorldLoader::Universe back;
    REQUIRE(Proto::DecodeUniverse(Proto::EncodeUniverse(u), back));
    CHECK(back.links.front().kind == "mouth");
    CHECK(back.links.front().maxMass == 30);
    CHECK(back.links.front().closesAt == 100);
    for (int p = 0; p <= Route::PREFERENCE_MAX; ++p)
        CHECK(Route::FindVia(back, "a", { "e", "b" }, 0, 0, (Route::Preference)p).path ==
              Route::FindVia(u, "a", { "e", "b" }, 0, 0, (Route::Preference)p).path);
}
