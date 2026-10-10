#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "entities/ShipType.h"
#include "core/Orbits.h"
#include "core/World.h"
#include "core/WorldLoader.h"
#include "entities/Entity.h"
#include "gen/Region.h"
#include "gen/Rng.h"

#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <string>

// The region from a seed (#140). Every rule in M7 is worthless if its answer changes between
// runs, machines or compilers, so that is what most of this file holds the generator to.

namespace
{
Gen::RegionParams Params(uint64_t seed)
{
    Gen::RegionParams p;
    p.seed = seed;
    p.homeId = "core";
    return p;
}

// Everything the generator decides, as one string.
std::string Dump(const Gen::Region& r)
{
    nlohmann::json all;
    all["systems"] = r.systems;
    all["links"] = r.links;
    all["entry"] = r.entryId;
    all["wormhole"] = r.wormhole;
    for (const auto& kv : r.documents)
        all["docs"][kv.first] = kv.second;
    return all.dump();
}

uint64_t Fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace

TEST_CASE("the generator's randomness is the same everywhere")
{
    // SplitMix64 from a published seed: if this changes, every galaxy changes.
    Gen::Rng r(1234567);
    CHECK(r.Next() == 6457827717110365317ull);
    CHECK(r.Next() == 3203168211198807973ull);

    // Ranges are inclusive and unbiased enough to reach both ends.
    Gen::Rng      s(42);
    std::set<int> seen;
    for (int i = 0; i < 2000; i++)
        seen.insert(s.Range(3, 7));
    CHECK(seen == std::set<int>{ 3, 4, 5, 6, 7 });
}

TEST_CASE("the same seed makes the same region, byte for byte")
{
    CHECK(Dump(Gen::GenerateRegion(Params(7))) == Dump(Gen::GenerateRegion(Params(7))));
    CHECK(Dump(Gen::GenerateRegion(Params(7))) != Dump(Gen::GenerateRegion(Params(8))));
}

TEST_CASE("the rules have not changed without saying so")
{
    // A golden value. CI computes it on three compilers (MinGW, GCC, Apple Clang): if it
    // differs between them, the generator is not deterministic across platforms, which is
    // the one thing it must be. If it differs after a change to the rules, that change made
    // a different galaxy from the same seed -- bump Gen::GENERATOR_VERSION (saves from the
    // old rules are then refused) and update the value here.
    const uint64_t h = Fnv1a(Dump(Gen::GenerateRegion(Params(1))));
    MESSAGE("region hash for seed 1: " << h);
    CHECK(Gen::GENERATOR_VERSION == 4);
    CHECK(h == 13600593835554633938ull);
}

TEST_CASE("every system can be reached from home, and every link has a gate on both ends")
{
    for (uint64_t seed : { 1ull, 2ull, 99ull, 123456789ull })
    {
        CAPTURE(seed);
        const Gen::Region r = Gen::GenerateRegion(Params(seed));
        REQUIRE(r.documents.size() == r.systems.size());

        std::map<std::string, std::vector<std::string>> adj;
        for (const auto& l : r.links)
        {
            adj[l[0]].push_back(l[1]);
            adj[l[1]].push_back(l[0]);
        }
        std::set<std::string>   reached = { "core" };
        std::deque<std::string> todo = { "core" };
        while (!todo.empty())
        {
            const std::string at = todo.front();
            todo.pop_front();
            for (const std::string& next : adj[at])
                if (reached.insert(next).second)
                    todo.push_back(next);
        }
        CHECK(reached.size() == r.systems.size() + 1);

        for (const auto& kv : r.documents)
        {
            CAPTURE(kv.first);
            std::set<std::string> dests;
            for (const auto& g : kv.second["gates"])
                dests.insert(g["destination"].get<std::string>());
            std::set<std::string> linked(adj[kv.first].begin(), adj[kv.first].end());
            CHECK(dests == linked);
        }
        CHECK(r.wormhole["destination"] == r.entryId);
    }
}

TEST_CASE("nothing generated sits in a planet's path or past the system's edge")
{
    int satellites = 0;
    for (uint64_t seed = 1; seed <= 40; seed++)
    {
        const Gen::Region r = Gen::GenerateRegion(Params(seed));
        for (const auto& kv : r.documents)
        {
            const nlohmann::json& sys = kv.second;
            for (const char* group : { "gates", "asteroidFields", "derelicts" })
                for (const auto& o : sys[group])
                {
                    CAPTURE(seed);
                    CAPTURE(kv.first);
                    CAPTURE(o.dump());
                    // What was in a planet's path is that planet's satellite (#210): close
                    // to it, clear of its surface, and never a gate.
                    if (o.contains("orbits"))
                    {
                        satellites++;
                        CHECK(std::string(group) != "gates");
                        CHECK_FALSE(o.contains("pos"));
                        const int i = o["orbits"]["planet"];
                        REQUIRE(i >= 0);
                        REQUIRE(i < (int)sys["planets"].size());
                        const double ps = sys["planets"][i]["size"];
                        const double radius = o["orbits"]["radius"], size = o["size"];
                        CHECK(radius - size > ps);
                        CHECK(radius + size <= ps * 1.5 + 12000.0);
                        continue;
                    }
                    const double x = o["pos"][0], y = o["pos"][1];
                    const double d = std::sqrt(x * x + y * y), size = o["size"];
                    CAPTURE(seed);
                    CAPTURE(kv.first);
                    CAPTURE(o.dump());
                    CHECK(d + size < World::SYSTEM_RADIUS);
                    for (const auto& p : sys["planets"])
                    {
                        const double R = p["orbitRadius"], ps = p["size"];
                        CHECK(std::fabs(d - R) >= ps + size);
                    }
                    // and not inside a star -- one in the middle, or a binary's two
                    if (sys.contains("star"))
                        CHECK(d > sys["star"]["size"].get<double>() + size);
                    if (sys.contains("stars"))
                        for (const auto& s : sys["stars"])
                        {
                            const double sx = s["pos"][0], sy = s["pos"][1];
                            CHECK(std::hypot(x - sx, y - sy) > s["size"].get<double>() + size);
                        }
                }
        }
    }
    CHECK(satellites > 0);  // the rule is not dead code
}

TEST_CASE("two satellites of one planet keep to their own orbits (#210)")
{
    for (uint64_t seed = 1; seed <= 40; seed++)
    {
        const Gen::Region r = Gen::GenerateRegion(Params(seed));
        for (const auto& kv : r.documents)
        {
            std::map<int, std::vector<std::pair<double, double>>> rings;  // planet -> [in, out]
            for (const char* group : { "asteroidFields", "derelicts" })
                for (const auto& o : kv.second[group])
                    if (o.contains("orbits"))
                    {
                        const double radius = o["orbits"]["radius"], size = o["size"];
                        rings[o["orbits"]["planet"].get<int>()].push_back(
                            { radius - size, radius + size });
                    }
            for (auto& pr : rings)
            {
                std::sort(pr.second.begin(), pr.second.end());
                for (size_t i = 1; i < pr.second.size(); i++)
                {
                    CAPTURE(seed);
                    CAPTURE(kv.first);
                    CHECK(pr.second[i].first > pr.second[i - 1].second);
                }
            }
        }
    }
}

TEST_CASE("a satellite goes round its planet, and the planet round its star, by the clock (#210)")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const nlohmann::json sys = nlohmann::json::parse(R"({
        "planets": [ { "type": "Gas", "size": 30000, "orbitRadius": 400000, "orbitSpeed": 400,
                       "angle": 0.0, "deposit": "Iron" } ],
        "derelicts": [ { "name": "Moonlet", "size": 50, "reward": 100,
                         "orbits": { "planet": 0, "radius": 45000, "speed": 90, "phase": 1.0 } } ]
    })");
    auto                 e = WorldLoader::BuildSystem(sys);
    REQUIRE(e.size() == 2);
    const Entity& planet = *e[0];
    const Entity& moon = *e[1];
    REQUIRE(moon.GetOrbit().has_value());
    // Built without a "pos": placed already, at world time zero.
    CHECK(planet.GetPosition().x == doctest::Approx(400000.0f));
    CHECK(Vector2Distance(moon.GetPosition(), planet.GetPosition()) == doctest::Approx(45000.0f));

    for (double t : { 0.0, 600.0, 86400.0, 604800.0 })
    {
        CAPTURE(t);
        Orbits::Place(e, t);
        CHECK(Vector2Length(planet.GetPosition()) == doctest::Approx(400000.0f).epsilon(1e-4));
        CHECK(Vector2Distance(moon.GetPosition(), planet.GetPosition()) ==
              doctest::Approx(45000.0f).epsilon(1e-3));
    }

    // A function of time, not of how often it was asked: one jump to an hour is the same
    // place as an hour of ticks.
    auto stepped = WorldLoader::BuildSystem(sys);
    for (int i = 1; i <= 3600; i++)
        Orbits::Place(stepped, i * 1.0);
    Orbits::Place(e, 3600.0);
    CHECK(stepped[1]->GetPosition().x == doctest::Approx(e[1]->GetPosition().x));
    CHECK(stepped[1]->GetPosition().y == doctest::Approx(e[1]->GetPosition().y));
}

TEST_CASE("a generated system is a system the game can build")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const Gen::Region r = Gen::GenerateRegion(Params(3));
    for (const auto& kv : r.documents)
    {
        CAPTURE(kv.first);
        const auto entities = WorldLoader::BuildSystem(kv.second);
        REQUIRE_FALSE(entities.empty());
        for (const auto& e : entities)
            CHECK(e->GetArchetype() != nullptr);  // every kind it used is a real one
    }
}

TEST_CASE("an object can say which archetype of its kind it is (#142)")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const nlohmann::json sys = nlohmann::json::parse(R"({
        "derelicts": [
            { "name": "Plain", "pos": [100, 0], "size": 40, "reward": 100 },
            { "name": "Named", "pos": [200, 0], "size": 40, "reward": 100, "archetype": "derelict.wreck" },
            { "name": "Typo", "pos": [300, 0], "size": 40, "reward": 100, "archetype": "derelict.nonesuch" },
            { "name": "Wrong kind", "pos": [400, 0], "size": 40, "reward": 100, "archetype": "gate.jump" }
        ]
    })");
    const auto           e = WorldLoader::BuildSystem(sys);
    REQUIRE(e.size() == 4);
    for (const auto& d : e)
    {
        // Every one is built -- a typo does not empty a system -- and every one ends up an
        // archetype of its own kind, never a gate's look on a wreck.
        REQUIRE(d->GetArchetype() != nullptr);
        CHECK(d->GetArchetype()->kind == EntityKind::Derelict);
    }
    CHECK(e[1]->GetArchetype()->id == "derelict.wreck");
}

TEST_CASE("rare finds are rare, unique in a region, and found by somebody (#211)")
{
    const char* FINDS[] = { "derelict.leviathan", "field.motherlode", "derelict.station_hulk",
                            "planet.rogue", "gate.ancient" };
    std::map<std::string, int> regionsWith;
    const int                  seeds = 60;
    for (uint64_t seed = 1; seed <= (uint64_t)seeds; seed++)
    {
        const Gen::Region          r = Gen::GenerateRegion(Params(seed));
        std::map<std::string, int> count;
        for (const auto& kv : r.documents)
            for (const char* group : { "derelicts", "asteroidFields", "planets", "gates" })
                for (const auto& o : kv.second[group])
                    if (o.contains("archetype"))
                        count[o["archetype"].get<std::string>()]++;
        for (const char* f : FINDS)
        {
            CAPTURE(seed);
            CAPTURE(f);
            // One of each at most -- an ancient gate is one gate with two mouths.
            CHECK(count[f] <= (std::string(f) == "gate.ancient" ? 2 : 1));
            if (count[f] > 0)
                regionsWith[f]++;
        }
        // An ancient gate is a way from deep in the region straight back to the first ring.
        for (const auto& kv : r.documents)
            for (const auto& g : kv.second["gates"])
                if (g.value("archetype", "") == std::string("gate.ancient"))
                    CHECK((Gen::DepthOf(kv.first) == 1 ||
                           Gen::DepthOf(g["destination"].get<std::string>()) == 1));
    }
    for (const char* f : FINDS)
    {
        CAPTURE(f);
        MESSAGE(std::string(f) << " in " << regionsWith[f] << " of " << seeds << " regions");
        CHECK(regionsWith[f] >= seeds / 5);       // somebody finds one
        CHECK(regionsWith[f] <= seeds * 9 / 10);  // and not everybody
    }
}

TEST_CASE("a satellite is a place the slowest ship can reach (#210)")
{
    // A station that moves faster than a ship can fly is a station nobody docks at: its
    // planet and its own turn together must stay well under the slowest hull's top speed.
    float slowest = 1e9f;
    for (const ShipType& t : GetShipCatalog())
        slowest = std::min(slowest, t.stats.maxSpeed);
    const double limit = slowest * 0.5;

    auto check = [&](const nlohmann::json& sys)
    {
        for (const char* group : { "stations", "asteroidFields", "derelicts", "nebulae" })
            if (sys.contains(group))
                for (const auto& o : sys[group])
                    if (o.contains("orbits"))
                    {
                        CAPTURE(o.dump());
                        const auto& p = sys["planets"][o["orbits"]["planet"].get<int>()];
                        CHECK(p["orbitSpeed"].get<double>() + o["orbits"].value("speed", 0.0) <
                              limit);
                    }
    };
    for (const char* name : { "core", "reach", "verge" })
    {
        CAPTURE(name);
        std::ifstream f(std::string(TEST_DATA_DIR) + "systems/" + name + ".json");
        REQUIRE(f.good());
        check(nlohmann::json::parse(f));
    }
    for (uint64_t seed = 1; seed <= 20; seed++)
        for (const auto& kv : Gen::GenerateRegion(Params(seed)).documents)
            check(kv.second);
}

TEST_CASE("what is out there is where it is for a reason (#146)")
{
    int remnants = 0, ruins = 0, ruinsByBelt = 0, ruinsInOrbit = 0, storied = 0, wrecks = 0;
    for (uint64_t seed = 1; seed <= 40; seed++)
        for (const auto& kv : Gen::GenerateRegion(Params(seed)).documents)
        {
            const nlohmann::json& sys = kv.second;
            for (const auto& b : sys["asteroidFields"])
                if (b["name"].get<std::string>().find("Remnant") != std::string::npos)
                {
                    remnants++;
                    CHECK(b.contains("pos"));  // between paths, never a planet's ring
                }
            for (const auto& d : sys["derelicts"])
            {
                const std::string name = d["name"];
                if (d.value("archetype", "") == std::string("derelict.outpost_ruin"))
                {
                    ruins++;
                    if (d.contains("orbits"))
                        ruinsInOrbit++;
                    else
                    {
                        // By a belt: the nearest belt is just beyond the clearance.
                        double nearest = 1e12;
                        for (const auto& b : sys["asteroidFields"])
                            if (b.contains("pos"))
                                nearest =
                                    std::min(nearest, std::hypot(d["pos"][0].get<double>() -
                                                                     b["pos"][0].get<double>(),
                                                                 d["pos"][1].get<double>() -
                                                                     b["pos"][1].get<double>()) -
                                                          b["size"].get<double>());
                        CAPTURE(kv.first);
                        CHECK(nearest < 45000.0);
                        ruinsByBelt++;
                    }
                }
                else if (!d.contains("archetype") && name.find("Frigate") == std::string::npos &&
                         name.find("Cruiser") == std::string::npos &&
                         name.find("Escort") == std::string::npos &&
                         name.find("Gunship") == std::string::npos &&
                         name.find("Bridge") == std::string::npos)
                {
                    wrecks++;
                    for (const char* s :
                         { "Prospector", "Survey", "Freighter", "Hauler", "Silent", "Courier" })
                        if (name.find(s) != std::string::npos)
                        {
                            storied++;
                            break;
                        }
                }
            }
        }
    MESSAGE("remnant belts " << remnants << ", ruins " << ruins << " (" << ruinsByBelt
                             << " by a belt, " << ruinsInOrbit << " in orbit), wrecks with a "
                             << "story " << storied << " of " << wrecks);
    CHECK(remnants > 0);
    CHECK(ruinsByBelt > 0);
    CHECK(ruinsInOrbit > 0);
    CHECK(storied * 2 > wrecks);  // most wrecks are where something went wrong
}
