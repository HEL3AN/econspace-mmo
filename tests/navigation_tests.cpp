#include <doctest/doctest.h>

#include "core/Navigation.h"
#include "sim/Orders.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// The map's itinerary (#432): planned as the server plans a route order, kept by the pilot
// whatever the order does, and restored paused.

namespace
{
using Path = std::vector<std::string>;

// a - c - e - d and a - b - d; b is low security.
WorldLoader::Universe Map()
{
    WorldLoader::Universe                            u;
    const std::vector<std::pair<std::string, float>> systems = {
        { "a", 1.0f }, { "b", 0.2f }, { "c", 0.9f }, { "d", 0.8f }, { "e", 0.5f }
    };
    for (const auto& [id, sec] : systems)
    {
        WorldLoader::SystemInfo s;
        s.id = s.name = id;
        s.security = sec;
        u.systems.push_back(s);
    }
    u.links = { { "a", "c" }, { "c", "e" }, { "e", "d" }, { "a", "b" }, { "b", "d" } };
    return u;
}

Proto::Snapshot At(const std::string& system, int order = 0,
                   Orders::Status     status = Orders::Status::Running,
                   const std::string& detail = "manual control")
{
    Proto::Snapshot s;
    s.player.orderDetail = detail;
    s.systemId = system;
    s.player.orderId = order;
    s.player.orderKind = order != 0 ? (int)Orders::Kind::Route : 0;
    s.player.orderStatus = (int)status;
    return s;
}
}  // namespace

TEST_CASE("an itinerary is a destination, its waypoints and a preference, planned as one route")
{
    const WorldLoader::Universe u = Map();
    Navigation                  n;
    std::string                 why;
    REQUIRE(n.SetDestination(u, "a", "d", 0, 0, why));
    CHECK(n.Path() == Path{ "a", "b", "d" });
    CHECK(n.State() == Navigation::Status::Planned);

    REQUIRE(n.SetPreference(u, "a", Route::Preference::Safer, 0, 0, why));
    CHECK(n.Path() == Path{ "a", "c", "e", "d" });

    // A waypoint goes before the destination, and the way is planned through it.
    REQUIRE(n.AddWaypoint(u, "a", "b", 0, 0, why));
    CHECK(n.Waypoints() == Path{ "b" });
    CHECK(n.Destination() == "d");
    CHECK(n.Path() == Path{ "a", "b", "d" });
    REQUIRE(n.RemoveWaypoint(u, "a", 0, 0, 0, why));
    CHECK(n.Path() == Path{ "a", "c", "e", "d" });

    // Somewhere with no way leaves the itinerary as it was, and says why.
    WorldLoader::SystemInfo island;
    island.id = "island";
    WorldLoader::Universe cut = u;
    cut.systems.push_back(island);
    CHECK_FALSE(n.SetDestination(cut, "a", "island", 0, 0, why));
    CHECK_FALSE(why.empty());
    CHECK(n.Destination() == "d");

    // A new destination is a new itinerary, as EVE's "set destination" is.
    REQUIRE(n.AddWaypoint(u, "a", "b", 0, 0, why));
    REQUIRE(n.SetDestination(u, "a", "e", 0, 0, why));
    CHECK(n.Waypoints().empty());
    CHECK(n.Preference() == Route::Preference::Safer);
}

TEST_CASE("an itinerary follows the ship, passes its waypoints, and keeps them through a stop")
{
    const WorldLoader::Universe u = Map();
    Navigation                  n;
    std::string                 why;
    REQUIRE(n.SetDestination(u, "a", "e", 0, 0, why));
    REQUIRE(n.AddWaypoint(u, "a", "b", 0, 0, why));
    CHECK(n.Path() == Path{ "a", "b", "d", "e" });

    n.Begin(4);
    CHECK(n.State() == Navigation::Status::Starting);
    n.Observe(At("a", 5), u, 0);
    CHECK(n.State() == Navigation::Status::Travelling);
    n.Observe(At("b", 5), u, 0);
    CHECK(n.Reached() == 1);
    CHECK(n.Waypoints().empty());  // passed, as the server's order passes it

    // The pilot takes the stick: the order fails as manual control, the itinerary is kept.
    CHECK(n.Observe(At("b", 5, Orders::Status::Failed), u, 0));
    CHECK(n.State() == Navigation::Status::Paused);
    CHECK(n.Destination() == "e");
    CHECK(n.Path() == Path{ "a", "b", "d", "e" });

    // ...and flies somewhere off the plan: the rest is planned from there, the past kept.
    n.Observe(At("a", 5, Orders::Status::Failed), u, 0);
    CHECK(n.Path().front() == "a");
    CHECK(n.Path()[n.Reached()] == "a");
    CHECK(n.Path().back() == "e");

    // Another order taking the ship pauses a journey under way.
    n.Begin(5);
    Proto::Snapshot dock = At("a", 6);
    dock.player.orderKind = (int)Orders::Kind::Dock;
    n.Observe(dock, u, 0);
    CHECK(n.State() == Navigation::Status::Paused);

    // Arriving is the snapshot's to say.
    n.Begin(6);
    n.Observe(At("e", 7, Orders::Status::Done), u, 0);
    CHECK(n.State() == Navigation::Status::Arrived);
    CHECK(n.Reached() == n.Path().size() - 1);
}

TEST_CASE("an itinerary is saved as it is and restored paused, and nonsense is refused")
{
    const WorldLoader::Universe u = Map();
    Navigation                  n;
    std::string                 why;
    REQUIRE(n.SetDestination(u, "a", "d", 0, 0, why));
    REQUIRE(n.AddWaypoint(u, "a", "e", 0, 0, why));
    REQUIRE(n.SetPreference(u, "a", Route::Preference::LessSecure, 0, 0, why));
    n.ToggleFavorite("c");
    n.Begin(1);
    n.Observe(At("a", 2), u, 0);
    REQUIRE(n.Active());

    Navigation back;
    REQUIRE(back.FromJson(n.ToJson()));
    CHECK(back.Destination() == "d");
    CHECK(back.Waypoints() == Path{ "e" });
    CHECK(back.Preference() == Route::Preference::LessSecure);
    CHECK(back.Path() == n.Path());
    CHECK(back.Favorite("c"));
    CHECK(back.State() == Navigation::Status::Paused);  // a journey resumes when asked

    Navigation     untouched = back;
    nlohmann::json bad = n.ToJson();
    bad["reached"] = 99;
    CHECK_FALSE(back.FromJson(bad));
    CHECK_FALSE(back.FromJson(nlohmann::json::array()));
    bad = n.ToJson();
    bad["preference"] = "fastest";
    CHECK_FALSE(back.FromJson(bad));
    CHECK(back.ToJson() == untouched.ToJson());

    // Clearing keeps the favourites and the preference: they are not the route.
    back.Clear();
    CHECK(back.Destination().empty());
    CHECK(back.State() == Navigation::Status::Empty);
    CHECK(back.Favorite("c"));
    CHECK(back.Preference() == Route::Preference::LessSecure);
}
